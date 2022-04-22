#pragma once

/*******************************************************************************
 * STD
 */
#include "memory"
#include "thread"

/*******************************************************************************
 * ROS
 */
#include "ros/ros.h"
#include "pluginlib/class_loader.h"
#include "mvp_control/ControlModes.h"
#include "mvp_control/ControlProcess.h"
#include "mvp_helm/GetState.h"
#include "mvp_helm/GetStates.h"
#include "mvp_helm/ChangeState.h"
/*******************************************************************************
 * Helm
 */
#include "behavior_container.h"
#include "obj.h"
#include "parser.h"
#include "sm.h"

namespace helm {

    class Helm : public HelmObj {
    private:

        /**
         * @brief Helm frequency in hertz
         */
        double m_helm_freq;

        /**
         * @brief Controller state
         * This variable holds the state of the low level controller such as
         * position & orientation.
         */
        mvp_control::ControlProcess::ConstPtr m_controller_process_values;

        /**
         * @brief Controller modes message object
         * Controller modes will be listened from low level controller and
         * stored in this variable
         */
        mvp_control::ControlModes m_controller_modes;

        /**
         * @brief Container for behaviors
         *
         */
        std::vector<std::shared_ptr<BehaviorContainer>> m_behavior_containers;

        /**
         * @brief Gets controller modes from low level controller
         *
         */
        void f_get_controller_modes();

        /**
         *
         * @param behavior_component
         */
        void f_generate_behaviors(
            const behavior_component_t& behavior_component);

        /**
         *
         * @param state
         */
        void f_generate_sm_states(const sm_state_t& state);

        /**
         *
         * @param conf
         */
        void f_configure_helm(helm_configuration_t conf);

        /**
         * @brief Initiates the plugins
         *
         */
        void f_initialize_behaviors();

        /**
         * @brief Parse object
         *
         */
        Parser::Ptr m_parser;

        /**
         * @brief State Machine object
         */
        StateMachine::Ptr m_state_machine;

        /**
         * @brief Executes one iteration of helm
         *
         */
        void f_iterate();


        /**
         * @brief Runs the #Helm::f_iterate function defined by
         * #Helm::m_helm_freq.
         */
        void f_helm_loop();


        /***********************************************************************
         * ROS - Publishers, Subscribers, Services and callbacks
         */

        //! @brief Controller state subscriber
        ros::Subscriber m_sub_controller_process_values;

        //! @brief Controller state request
        ros::Publisher m_pub_controller_set_point;

        /**
         * @brief Topic callback for state
         * @param msg
         */
        void f_cb_controller_process(
            const mvp_control::ControlProcess::ConstPtr& msg);

        ros::ServiceServer m_change_state_srv;

        ros::ServiceServer m_get_states_srv;

        ros::ServiceServer m_get_state_srv;

        bool f_cb_change_state(
            mvp_helm::ChangeState::Request& req,
            mvp_helm::ChangeState::Response& resp);

        bool f_cb_get_state(
            mvp_helm::GetState::Request& req,
            mvp_helm::GetState::Response& resp);

        bool f_cb_get_states(
            mvp_helm::GetStates::Request& req,
            mvp_helm::GetStates::Response& resp);

        bool f_change_state(std::string name);

    public:

        /**
         * @brief Trivial constructor
         */
        Helm();

        /**
         * @brief Initialize Helm
         */
        void initialize();

        /**
         * @brief Run the Helm
         */
        void run();

        ~Helm();

    };
}