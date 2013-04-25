/*
 * VisualServoing2D.cpp
 *
 *  Created on: Apr 22, 2013
 *      Author: badrobot
 */

#include "VisualServoing2D.h"

VisualServoing2D::VisualServoing2D( bool debugging,
									ros::ServiceClient safe_cmd_vel_service,
									std::vector<std::string> arm_joint_names,
									std::vector<arm_navigation_msgs::JointLimits> arm_joint_limits )
{
	g_debugging = debugging;

	m_first_pass = true;
	m_done_base_x_adjustment = true;
	m_done_base_y_adjustment = true;
	m_done_arm_rot_adjustment = true;
	m_blob_detection_completed = false;

	m_background_image = LoadBackgroundImage();

	CreatePublishers( 1 );

	m_arm_joint_names = arm_joint_names;
	m_arm_joint_limits = arm_joint_limits;

	m_safe_cmd_vel_service = safe_cmd_vel_service;

	if( g_debugging )
	{
		ROS_INFO( "Debugging Enabled" );
	}
}

VisualServoing2D::~VisualServoing2D()
{
	cvDestroyWindow( "Original" );
	cvDestroyWindow( "Thresholding" );
	cvDestroyWindow( "Found Blobs" );
	cvDestroyWindow( "Background Image" );
}

bool
VisualServoing2D::VisualServoing( IplImage* input_image )
{
	double x_offset = 0;
	double y_offset = 0;
	double rot_offset = 0;

	IplImage* cv_image  ;
	IplImage* blob_image;

	CBlobGetOrientation get_orientation;

	CBlob   temp_tracked_blob;
	double  temp_tracked_blob_distance = 0;

	double maxx;
	double minx;
	double maxy;
	double miny;
	double temp_x;
	double temp_y;
	double dist_x;
	double dist_y;
	double distance;

	// Covert the image from a ROS image message to a OpenCV Image (IplImage) type.
	cv_image = input_image;
	if( !cv_image )
	{
		ROS_ERROR( "Error in input image!" );
		return false;
	}

	// TODO: Modify the program so that it can still run without a background image!
	IplImage* background_threshold = cvCreateImage( cvGetSize( m_background_image ), 8, 1 );
	cvCvtColor( m_background_image, background_threshold, CV_BGR2GRAY );
	cvSmooth( background_threshold, background_threshold, CV_GAUSSIAN, 7, 7 );
	cvThreshold( background_threshold, background_threshold, 0, 255, CV_THRESH_BINARY_INV | CV_THRESH_OTSU );

	blob_image = cvCreateImage( cvGetSize( cv_image ), IPL_DEPTH_8U, cv_image->nChannels );

	IplImage* gray = cvCreateImage( cvGetSize( cv_image ), IPL_DEPTH_8U, 1 );
	cvCvtColor( cv_image, gray, CV_BGR2GRAY );
	cvSmooth( gray, gray, CV_GAUSSIAN, 7, 7 );
	//cvEqualizeHist( gray, gray );
	cvThreshold( gray, gray, 0, 255, CV_THRESH_BINARY_INV | CV_THRESH_OTSU );

	//    This takes a background image (the gripper on a white background) and removes
	//  it from the current image (cv_image). The results are stored again in cv_image.
	cvSub( gray, background_threshold, gray, NULL );

	// Find any blobs that are not white.
	CBlobResult blobs = CBlobResult( gray, NULL, 0 );

	blobs.Filter( blobs, B_EXCLUDE, CBlobGetArea(), B_LESS, m_min_blob_area );
	blobs.Filter( blobs, B_EXCLUDE, CBlobGetArea(), B_GREATER, m_max_blob_area );

	//  We will only grab the largest blob on the first pass from that point on we will look for the centroid
	//  of a blob that is closest to the centroid of the largest blob.
	if( m_first_pass == true )
	{
	  ROS_DEBUG( "First pass through visual servoing." );

	  CBlob largest_blob;
	  blobs.GetNthBlob( CBlobGetPerimeter(), 0, largest_blob );
	  maxx = largest_blob.MaxX();
	  minx = largest_blob.MinX();
	  maxy = largest_blob.MaxY();
	  miny = largest_blob.MinY();
	  m_tracked_x = ( ( minx + maxx ) / 2 );
	  m_tracked_y = ( ( miny + maxy ) / 2 );

	  m_first_pass = false;
	}

	//  Go through all of the blobs and find the one that is the closest to the previously tracked blob.
	for( int x = 0; x < blobs.GetNumBlobs(); x++ )
	{
	  CBlob  temp_blob;

	  temp_blob = blobs.GetBlob( x );

	  maxx = temp_blob.MaxX();
	  minx = temp_blob.MinX();
	  maxy = temp_blob.MaxY();
	  miny = temp_blob.MinY();
	  temp_x = ( ( minx + maxx ) / 2 );
	  temp_y = ( ( miny + maxy ) / 2 );
	  dist_x = ( temp_x ) - ( m_tracked_x );
	  dist_y = ( temp_y ) - ( m_tracked_y );
	  distance = sqrt( ( dist_x * dist_x ) + ( dist_y * dist_y ) );

	  if( temp_tracked_blob_distance == 0 )
	  {
		temp_tracked_blob = temp_blob;
		temp_tracked_blob_distance = distance;
		m_tracked_x = temp_x;
		m_tracked_y = temp_y;
	  }
	  else
	  {
		if( distance < temp_tracked_blob_distance )
		{
		  temp_tracked_blob = temp_blob;
		  temp_tracked_blob_distance = distance;
		  m_tracked_x = temp_x;
		  m_tracked_y = temp_y;
		}
	  }

	  //temp_blob.FillBlob( blob_image, CV_RGB( 255, 0, 0 ) );
	}

	if( g_debugging )
	{
		//  Draw the blob we are tracking as well as a circle to represent the centroid of that object.
		temp_tracked_blob.FillBlob( blob_image, CV_RGB( 0, 0, 255 ) );
		cvCircle( blob_image, cvPoint( m_tracked_x, m_tracked_y ), 10, CV_RGB( 255, 0, 0 ), 2 );
	}

	x_offset = ( m_tracked_x ) - ( m_image_width / 2 );
	y_offset = ( m_tracked_y ) - ( (m_image_height/2) + m_verticle_offset );
	rot_offset = get_orientation( temp_tracked_blob );
	if( rot_offset > 180 )
	{
	  rot_offset = rot_offset - 180;
	}

	BaseAdjustmentX( x_offset );
	BaseAdjustmentY( y_offset );
	ArmAdjustment( rot_offset );

	if( m_done_base_x_adjustment == true && m_done_base_y_adjustment == true && m_done_arm_rot_adjustment == true )
	{
		m_blob_detection_completed = true;
		return true;
		ROS_INFO( "Visual Servoing Completed." );
	}


	if( g_debugging )
	{
		// Setting up fonts for overlay information.
		CvFont font;
		cvInitFont(&font, CV_FONT_HERSHEY_SIMPLEX, 1.0, 1.0, 0, 1, CV_AA);

		cvLine( blob_image,   cvPoint( 0, (m_image_height/2) + m_verticle_offset ), cvPoint( m_image_width, (m_image_height/2) + m_verticle_offset ), CV_RGB( 255, 0, 0 ), 2, 0 );
		cvLine( blob_image,   cvPoint( (m_image_width/2), 0 ), cvPoint( (m_image_width/2), m_image_height ), CV_RGB( 255, 0, 0 ), 2, 0 );
		cvRectangle( blob_image, cvPoint( 0, blob_image->height-40 ), cvPoint( blob_image->width, blob_image->height ), CV_RGB( 0, 0, 0 ), -1 );

		std::string x_str = "X: ";
		x_str += boost::lexical_cast<std::string>( x_offset );

		std::string y_str = "Y: ";
		y_str += boost::lexical_cast<std::string>( y_offset );

		std::string rot_str = "Rotation: ";
		rot_str += boost::lexical_cast<std::string>( rot_offset );

		cvPutText( blob_image, x_str.c_str(), cvPoint( 10, blob_image->height - 10 ), &font, CV_RGB( 255, 0, 0 ) );
		cvPutText( blob_image, y_str.c_str(),  cvPoint( 185, blob_image->height - 10 ), &font, CV_RGB( 255, 0, 0 ) );
		cvPutText( blob_image, rot_str.c_str(), cvPoint( 350, blob_image->height - 10 ), &font, CV_RGB( 255, 0, 0 ) );

		HUD("b-it-bots Visual Servoing", 3, cv_image, m_background_image, blob_image );
		cvSetZero( blob_image );
		cvWaitKey( 10 );
	}


	cvSetZero( gray );
	cvSetZero( cv_image );
	cvSetZero( blob_image );

	cvReleaseImage( &gray );
	cvReleaseImage( &blob_image );
}

void
VisualServoing2D::BaseAdjustmentX( double x_offset )
{
	double move_speed = 0.0;

	if( x_offset > m_x_threshold )
	{
		// move the robot base right
		move_speed = -m_x_velocity;
		m_done_base_x_adjustment = false;
	}
	else if( x_offset < -m_x_threshold )
	{
		// move the robot left
		move_speed = m_x_velocity;
		m_done_base_x_adjustment = false;
	}
	else if( fabs( x_offset ) < m_x_threshold )
	{
		move_speed = 0.0;
		m_done_base_x_adjustment = true;
		ROS_INFO( "Base Adjustment in X Finished" );
	}
	else
	{
		// should never happen but just in case.
		move_speed = 0.0;
	}

	// Prepare and then send the base movement commands.
	m_youbot_base_velocities.linear.y = move_speed;
	m_base_velocities_publisher.publish( m_youbot_base_velocities );
}

void
VisualServoing2D::BaseAdjustmentY( double y_offset )
{
	double move_speed = 0.0;

	if( !m_safe_cmd_vel_service.call( m_service_msg ) )
	{
		ROS_ERROR( "Visual Servoing call to is_robot_to_close_to_obstacle has failed" );
		m_service_msg.response.value = false;
	}

	if( y_offset >= m_y_threshold )
	{
		// move the robot base right
		move_speed = -m_y_velocity;
		m_done_base_y_adjustment = false;
	}
	else if( y_offset <= -m_y_threshold )
	{
		// move the robot left
		move_speed = m_y_velocity;
		m_done_base_y_adjustment = false;
	}
	else if( fabs( y_offset ) < m_y_threshold )
	{
		move_speed = 0.0;
		m_done_base_y_adjustment = true;
		ROS_INFO( "Base Adjustment in Y Finished" );
	}
	else if( m_service_msg.response.value == true )
	{
		// This will only be set when the safe_cmd_vel is telling us that it cannot
		//  allow for movement any longer in this direction.
		move_speed = 0.0;
		m_done_base_y_adjustment = true;
		ROS_INFO( "Base Adjustment in Y Finished" );
	}
	else
	{
		// should never happen but just in case.
		m_done_base_y_adjustment = true;
		move_speed = 0.0;
	}

	// Prepare and then send the base movement commands.
	m_youbot_base_velocities.linear.x = move_speed;
	m_base_velocities_publisher.publish( m_youbot_base_velocities );
}

bool
VisualServoing2D::ArmAdjustment( double orientation )
{
	double difference = fabs( orientation - m_rot_target );
	double rotational_speed = 0.0;


	if( orientation > m_rot_target && difference > m_rot_tolerance )
	{
		/**
		 * We are not to far to the right of the object and our difference is not small enough yet.
		 */
		rotational_speed = m_rot_velocity;
		m_done_arm_rot_adjustment = false;
	}
	else if( orientation < m_rot_target && difference > m_rot_tolerance )
	{
		/**
		 * we are to far to the left of the object and our difference is still to large.
		 */
		rotational_speed = -m_rot_velocity;
		m_done_arm_rot_adjustment = false;
	}
	else if( difference < m_rot_tolerance )
	{
		rotational_speed = 0.0;
		m_done_arm_rot_adjustment = true;
		ROS_INFO( "Arm Rotation Finished" );
	}
	else
	{
		/**
		 * We should never arrive at this state but just encase.
		 */
		ROS_ERROR( "SHIT WENT WRONG!" );
		m_done_arm_rot_adjustment = false;
	}

	ROS_INFO( "Orientation\t%f", orientation );
	ROS_INFO( "Difference\t%f", difference );

	/**
	 * we need to loop though all of the joint states because we need to set anything we do not want
	 * to move to 0. If we do not do this we could get uncontrolled movements from values that it
	 * had previously been sent.
	 */
	m_youbot_arm_velocities.velocities.clear();
	for(unsigned int i=0; i < m_arm_joint_names.size(); ++i)
	{
		brics_actuator::JointValue joint_value;

		joint_value.timeStamp = ros::Time::now();
		joint_value.joint_uri = m_arm_joint_names[i];
		joint_value.unit = to_string(boost::units::si::radian_per_second);

		if( i == 4 )
		{
		  joint_value.value = rotational_speed;
		}
		else
		{
		  joint_value.value = 0.0;
		}

		m_youbot_arm_velocities.velocities.push_back(joint_value);
		//m_arm_velocities_publisher.publish( m_youbot_arm_velocities );
	}

	return m_done_arm_rot_adjustment;


	/*
	if( rot_offset != 90 || rot_offset != 270 )
	{
		double rotational_speed = 0.0;



		if( ( rot_offset < 85 && rot_offset >= 0 ) || ( rot_offset < 265 && rot_offset >= 235 ) )
		{
			rotational_speed = -m_rot_velocity;
			m_done_arm_rot_adjustment = false;
		}
		else if( rot_offset > 94 && rot_offset < 235 )
		{
			rotational_speed = m_rot_velocity;
			m_done_arm_rot_adjustment = false;
		}
		else
		{
			rotational_speed = 0.0;
			m_done_arm_rot_adjustment = true;
		}

		m_youbot_arm_velocities.velocities.clear();
		for(unsigned int i=0; i < m_arm_joint_names.size(); ++i)
		{
			brics_actuator::JointValue joint_value;

			joint_value.timeStamp = ros::Time::now();
			joint_value.joint_uri = m_arm_joint_names[i];
			joint_value.unit = to_string(boost::units::si::radian_per_second);

			if( i == 4 )
			{
			  joint_value.value = rotational_speed;
			}
			else
			{
			  joint_value.value = 0.0;
			}

			m_youbot_arm_velocities.velocities.push_back(joint_value);
			//m_arm_velocities_publisher.publish( m_youbot_arm_velocities );
		}
	}
	*/
}

IplImage*
VisualServoing2D::LoadBackgroundImage()
{
	IplImage* background_image;
	try
	{
	  std::string package_path = ros::package::getPath("raw_visual_servoing") + "/common/data/background.png";
	  std::cout << "Package Path:\t" << package_path.c_str() << std::endl;
	  background_image = cvLoadImage( package_path.c_str() );
	}
	catch ( cv::Exception& e )
	{
		ROS_ERROR( "Could not load background image" );
	}

	return background_image;
}

void
VisualServoing2D::CreatePublishers( int arm_model )
{
	// Set up the base velocities publisher:
	m_base_velocities_publisher = m_node_handler.advertise<geometry_msgs::Twist>( "/cmd_vel_safe", 1 );

	ROS_INFO( "Robot Base Publisher Setup" );

	if( arm_model == 0 )
	{
		ROS_INFO( "The Visual Servoing cannot move the arm" );
	}
	else if( arm_model == 1 )
	{
		m_arm_velocities_publisher = m_node_handler.advertise<brics_actuator::JointVelocities>( "/arm_controller/velocity_command", 1 );
		ROS_INFO( "KUKA YouBot Arm Publisher is set up" );
	}
	else if( arm_model == 2 )
	{
		ROS_ERROR( "KUKA Lightwieght Arm has not been implemented" );
	}
	else
	{
		ROS_ERROR( "Unkown robotic arm model provided" );
	}
}

void
VisualServoing2D::HUD(char* title, int nArgs, ...) {

    // img - Used for getting the arguments
    IplImage *img;

    // DispImage - the image in which input images are to be copied
    IplImage *DispImage;

    int size;
    int i;
    int m, n;
    int x, y;

    // w - Maximum number of images in a row
    // h - Maximum number of images in a column
    int w, h;

    // scale - How much we have to resize the image
    float scale;
    int max;

    // If the number of arguments is lesser than 0 or greater than 12
    // return without displaying
    if(nArgs <= 0) {
        printf("Number of arguments too small....\n");
        return;
    }
    else if(nArgs > 12) {
        printf("Number of arguments too large....\n");
        return;
    }
    // Determine the size of the image,
    // and the number of rows/cols
    // from number of arguments
    else if (nArgs == 1) {
        w = h = 1;
        size = 300;
    }
    else if (nArgs == 2) {
        w = 2; h = 1;
        size = 300;
    }
    else if (nArgs == 3 || nArgs == 4) {
        w = 2; h = 2;
        size = 300;
    }
    else if (nArgs == 5 || nArgs == 6) {
        w = 3; h = 2;
        size = 200;
    }
    else if (nArgs == 7 || nArgs == 8) {
        w = 4; h = 2;
        size = 200;
    }
    else {
        w = 4; h = 3;
        size = 150;
    }

    // Create a new 3 channel image
    DispImage = cvCreateImage( cvSize( 50 + size*w, 60 + size*h), 8, 3 );

    // Used to get the arguments passed
    va_list args;
    va_start(args, nArgs);

    // Loop for nArgs number of arguments
    for (i = 0, m = 20, n = 20; i < nArgs; i++, m += (20 + size)) {

        // Get the Pointer to the IplImage
        img = va_arg(args, IplImage*);

        // Check whether it is NULL or not
        // If it is NULL, release the image, and return
        if(img == 0) {
            printf("Invalid arguments");
            cvReleaseImage(&DispImage);
            return;
        }

        // Find the width and height of the image
        x = img->width;
        y = img->height;

        // Find whether height or width is greater in order to resize the image
        max = (x > y)? x: y;

        // Find the scaling factor to resize the image
        scale = (float) ( (float) max / size );

        // Used to Align the images
        if( i % w == 0 && m!= 20) {
            m = 20;
            n+= 20 + size;
        }

        // Set the image ROI to display the current image
        cvSetImageROI(DispImage, cvRect(m, n, (int)( x/scale ), (int)( y/scale )));

        // Resize the input image and copy the it to the Single Big Image
        cvResize( img, DispImage );

        // Reset the ROI in order to display the next image
        cvResetImageROI(DispImage);
    }

    // Create a new window, and show the Single Big Image
    cvNamedWindow( title, 1 );
    cvShowImage( title, DispImage);

    //cvWaitKey();
    //cvDestroyWindow(title);

    // End the number of arguments
    va_end(args);

    // Release the Image Memory
    //cvReleaseImage(&DispImage);
}
