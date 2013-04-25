/**
 * This is the main source code file for running the Visual Servoing application. This application 
 * is dependent on the Robot Operating System (ROS) [www.ros.org], and will not run unless the
 * required ROS components are installed. You can do this by running the "rosdep install" command
 * from the base folder.
 *
 * This file is provided as is without any form or warranty.
 *
 * Author: Matthew Roscoe (mat.roscoe@unb.ca)
 */
#include <ros/ros.h>
#include <image_transport/image_transport.h>
#include <opencv/cv.h>
#include <opencv/highgui.h>
#include <cv_bridge/CvBridge.h>
#include <sensor_msgs/JointState.h>
#include <kdl/kdl.hpp>
#include <kdl/chainiksolvervel_wdls.hpp>

#include <std_srvs/Empty.h>
#include <hbrs_srvs/ReturnBool.h>

#include <arm_navigation_msgs/JointLimits.h>
#include <brics_actuator/JointVelocities.h>
#include <brics_actuator/JointPositions.h>

#include "geometry_msgs/Twist.h"

#include "VisualServoing2D.h"

class raw_visual_servoing 
{
public:
	/**
	 * This is the constructor for the visual seroving application. It will start the advertising of
	 * the visual seroving service so that the process can be started and stopped on command. If you
	 * want to start the visual servoing you need to run the do_visual_servoing service hook.
	 */
	raw_visual_servoing( ros::NodeHandle &n ) : m_node_handler( n ),
												image_transporter( n )
	{
		m_visual_servoing = new VisualServoing2D( true,
												  safe_cmd_vel_service,
												  m_arm_joint_names );

		SetupYoubotArm();

		// Service commands to allow this node to be started and stopped externally
		service_do_visual_serv = m_node_handler.advertiseService( "do_visual_servoing", &raw_visual_servoing::do_visual_servoing, this );
		ROS_INFO( "Advertised 'do_visual_servoing' service for raw_visual_servoing" );

		ROS_INFO( "Visual servoing node initialized." );
	}

	/**
	 * Standard destructor.
	 */
	~raw_visual_servoing()
	{
	}

	/**
	 * This is the service hook for visual servoing. If you want to run the acutal visual servoing
	 * you wll need to call the "do_visual_servoing" service call through ROS.
	 */
	bool do_visual_servoing( hbrs_srvs::ReturnBool::Request &req,
							 hbrs_srvs::ReturnBool::Response &res )
	{
		m_is_visual_servoing_completed = false;

		//  Incoming message from raw_usbs_cam. This must be running in order for this ROS node to run.
		m_image_subscriber = image_transporter.subscribe( "/usb_cam/image_raw", 1, &raw_visual_servoing::imageCallback, this );

		// get joint states and store them to a variable and go through them (arm_link_5) and check to see if the current state is
		// to close to the min or max value.
		m_sub_joint_states = m_node_handler.subscribe( "/joint_states", 1, &raw_visual_servoing::jointstateCallback, this );

		safe_cmd_vel_service = m_node_handler.serviceClient<hbrs_srvs::ReturnBool>("/is_robot_to_close_to_obstacle");

		// Velocity control for the YouBot base.
		base_velocities_publisher = m_node_handler.advertise<geometry_msgs::Twist>( "/cmd_vel_safe", 1 );

		// Velocity Control for the YouBot arm.
		//arm_velocities_publisher = node_handler.advertise<brics_actuator::JointVelocities>( "/arm_controller/velocity_command", 1 );

		ros::Time start_time = ros::Time::now();

		ROS_INFO("VisualServoing: Starting Blob Detection");

		while( ( m_is_visual_servoing_completed == false ) && ros::ok() && ( (ros::Time::now() - start_time).toSec() < m_visual_servoing_timeout ) )
		{
			//ROS_INFO( "Timeout: %f", ros::Time::now() - start_time  );
			ros::spinOnce();
		}

		if( (ros::Time::now() - start_time).toSec() < m_visual_servoing_timeout )
		{
			ROS_INFO( "Visual Servoing Sucessful." );
			res.value = true;
		}
		else
		{
			ROS_ERROR( "Visual Servoing Failure due to Timeout" );
			res.value = false;
			geometry_msgs::Twist zero_vel;
			base_velocities_publisher.publish(zero_vel);
		}

		// Turn off the image subscriber for the web camera.
		m_image_subscriber.shutdown();

		// Turn off the velocity publishers for the YouBot Arm & Base.
		arm_velocities_publisher.shutdown();
		base_velocities_publisher.shutdown();

		// Shut down any open windows.
		cvDestroyAllWindows();

		return true;
	}

	/**
	 * This is the service call that is used to stop the visual servoing from running. It will only
	 * turn off the subscribers and publishers but keep libraries loaded if they are required later
	 * on. In order to reduce memory footprint we also close any currently open HighGUI windows.
	 */
	bool stop(std_srvs::Empty::Request &req, std_srvs::Empty::Response &res)
	{
		// Turn off the image subscriber for the web camera.
		m_image_subscriber.shutdown();

		// Turn off the velocity publishers for the YouBot Arm & Base.
		arm_velocities_publisher.shutdown();
		base_velocities_publisher.shutdown();

		// Shut down any open windows.
		cvDestroyAllWindows();

		ROS_INFO("Blob Detection Disabled");
		return true;
	}

private:
  /**
   * This function is responsible for querying the parameter server that is currently running
   * for any robotic arm parameters that relate directly to the KUKA YouBot.
   */
  void SetupYoubotArm()
  {
	  XmlRpc::XmlRpcValue parameter_list;
	  m_node_handler.getParam("/arm_controller/joints", parameter_list);
	  ROS_ASSERT(parameter_list.getType() == XmlRpc::XmlRpcValue::TypeArray);

	  for (int32_t i = 0; i < parameter_list.size(); ++i)
	  {
		ROS_ASSERT(parameter_list[i].getType() == XmlRpc::XmlRpcValue::TypeString);
		m_arm_joint_names.push_back(static_cast<std::string>(parameter_list[i]));
	  }

	  //read joint limits
	  for(unsigned int i=0; i < m_arm_joint_names.size(); ++i)
	  {
		arm_navigation_msgs::JointLimits joint_limits;
		joint_limits.joint_name = m_arm_joint_names[i];
		m_node_handler.getParam("/arm_controller/limits/" + m_arm_joint_names[i] + "/min", joint_limits.min_position);
		m_node_handler.getParam("/arm_controller/limits/" + m_arm_joint_names[i] + "/max", joint_limits.max_position);
		//m_arm_joint_limits.push_back(joint_limits);
		m_upper_joint_limits.push_back( joint_limits.max_position );
		m_lower_joint_limits.push_back( joint_limits.min_position );
	  }
  }

  /**
   * This function is responsible for calling the libraries that will perform the visual servoing
   * on the image that is coming in from either the 2D or 3D camera depending on which sensors are
   * currently available to the user.
   */
  void imageCallback( const sensor_msgs::ImageConstPtr& image_message )
  	{
  		sensor_msgs::CvBridge bridge;
  		IplImage *cv_image = NULL;

  		try
  		{
  			cv_image = bridge.imgMsgToCv( image_message, "bgr8" );
  		}
  		catch( sensor_msgs::CvBridgeException& e )
  		{
  			ROS_ERROR( "Could not convert from '%s' to 'bgr8'.", image_message->encoding.c_str() );
  		}

  		m_is_visual_servoing_completed = m_visual_servoing->VisualServoing( cv_image );
  		m_is_visual_servoing_completed = checkLimits( m_joint_positions );
  	}

  void jointstateCallback( sensor_msgs::JointStateConstPtr joints )
  {

  	for (unsigned i = 0; i < joints->position.size(); i++) {

  		const char* joint_uri = joints->name[i].c_str();

  		for (unsigned int j = 0; j < m_arm_chain.getNrOfJoints(); j++) {
  			const char* chainjoint =
  					m_arm_chain.getSegment(j).getJoint().getName().c_str();

  			if (chainjoint != 0 && strcmp(chainjoint, joint_uri) == 0) {
  				m_joint_positions.data[j] = joints->position[i];
  				m_joint_positions_initialized[j] = true;
  			}
  		}
  	}
  }

  bool
  checkLimits( KDL::JntArray joint_positions )
  {
	  const double joint_threshold = 0.05;

	  if( m_upper_joint_limits.size() < m_arm_chain.getNrOfJoints() ||
		  m_lower_joint_limits.size() < m_arm_chain.getNrOfJoints())
	  {
		  ROS_ERROR( "No Joint Limits Defined" );
		  return false;
	  }

	  for( unsigned int x = 0; x < m_arm_chain.getNrOfJoints(); x++)
	  {
		  double diff_up = fabs( (double)joint_positions.data(x) - m_upper_joint_limits[x] );
		  double diff_dn = fabs( (double)joint_positions.data(x) - m_lower_joint_limits[x] );

		  if( diff_up < joint_threshold || diff_dn < joint_threshold )
		  {
			  ROS_ERROR( "Joint soft-limit reached" );
			  return false;
		  }
	  }
	  ROS_INFO( "Joint states okay" );
	  return true;
  }

protected:

  VisualServoing2D*									m_visual_servoing;

  ros::NodeHandle 									m_node_handler;
  image_transport::ImageTransport 					image_transporter;
  image_transport::Subscriber 						m_image_subscriber;

  ros::ServiceClient  								safe_cmd_vel_service;

  ros::Publisher								 	base_velocities_publisher;
  ros::Publisher 									arm_velocities_publisher;

  ros::Subscriber 									m_sub_joint_states;

  std::vector<std::string> 							m_arm_joint_names;
  std::vector<double> 								m_upper_joint_limits;
  std::vector<double> 								m_lower_joint_limits;
  KDL::JntArray 									m_joint_positions;
  std::vector<bool> 								m_joint_positions_initialized;

  ros::ServiceServer 								service_do_visual_serv;

  bool 												m_is_visual_servoing_completed;

  const static int 									m_visual_servoing_timeout = 30;

  KDL::Chain 										m_arm_chain;
};

/**
 * The main function for our visual servoing application. This will launch all of the components
 * required to perform the visual servoing.
 */
int main(int argc, char** argv)
{
  ros::init(argc, argv, "image_listener");
  ros::NodeHandle n;
  raw_visual_servoing ic(n);
  ros::spin();
  return 0;
}
