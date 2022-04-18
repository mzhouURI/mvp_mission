
/*******************************************************************************
 * STD
 */
#include "functional"
#include "utility"

/*******************************************************************************
 * ROS
 */
#include "mvp_control/GetControlModes.h"
#include "mvp_control/dictionary.h"

/*******************************************************************************
 * Helm
 */
#include "helm.h"

#include <utility>
#include "behavior_container.h"
#include "dictionary.h"
#include "utils.h"
#include "exception.h"

/*******************************************************************************
 * namespaces
 */
using namespace helm;

/*******************************************************************************
 * Implementations
 */


/*******************************************************************************
 * Public methods
 */

Helm::Helm() : HelmObj() {

};

Helm::~Helm() {

    m_sub_controller_process_values.shutdown();

    m_pub_controller_set_point.shutdown();

}

void Helm::initialize() {

    /***************************************************************************
     * Initialize objects
     */

    m_parser.reset(new Parser());

    m_state_machine.reset(new StateMachine());

    /***************************************************************************
     * Parse mission file
     */

    m_parser->set_op_behavior_component(std::bind(
        &Helm::f_generate_behaviors, this, std::placeholders::_1
    ));

    m_parser->set_op_sm_component(std::bind(
        &Helm::f_generate_sm_states, this, std::placeholders::_1
    ));

    m_parser->set_op_helmconf_component(std::bind(
        &Helm::f_configure_helm, this, std::placeholders::_1
    ));

    m_parser->initialize();

    /***************************************************************************
     * Initialize subscriber
     *
     */
    m_sub_controller_process_values = m_nh->subscribe(
        ctrl::TOPIC_CONTROL_PROCESS_VALUE,
        100,
        &Helm::f_cb_controller_state,
        this
    );

    m_pub_controller_set_point = m_nh->advertise<mvp_control::ControlProcess>(
        ctrl::TOPIC_CONTROL_PROCESS_SET_POINT,
        100
    );

    /***************************************************************************
     * Initialize state machine
     */
    m_state_machine->initialize();

    /***************************************************************************
     * Initialize behavior plugins
     */
    f_initialize_behaviors();


    /***************************************************************************
     * setup connection with low level controller
     */
    f_get_controller_modes();

}

void Helm::run() {

    std::thread helm_loop([this] { f_helm_loop(); });

    ros::spin();

    helm_loop.join();
}

/*******************************************************************************
 * Private methods
 */

void Helm::f_initialize_behaviors() {

    for(const auto& i : m_behavior_containers) {
        i->initialize();

        i->get_behavior()->set_helm_frequency(m_helm_freq);
    }

}

void Helm::f_get_controller_modes() {

    auto client = m_nh->serviceClient
        <mvp_control::GetControlModes>(ctrl::SERVICE_GET_CONTROL_MODES);

    while(!client.waitForExistence(ros::Duration(5))) {
        ROS_WARN_STREAM(
            "Waiting for service: " << client.getService()
        );
    }

    mvp_control::GetControlModes srv;
    client.call(srv);
    m_controller_modes.modes = srv.response.modes;

}

void Helm::f_generate_behaviors(const behavior_component_t& component)
{
    /**
     * This function is called each time parser reads a tag defined by
     * #helm::xml::bhvconf::behavior::TAG. This function is defined so that
     * initiating of the objects are done by an #helm::Helm object.
     */

    BehaviorContainer::Ptr b = std::make_shared<BehaviorContainer>(
        component
    );

    m_behavior_containers.emplace_back(b);

}

void Helm::f_generate_sm_states(const sm_state_t& state) {

    m_state_machine->append_state(state);

}

void Helm::f_configure_helm(helm_configuration_t conf) {

    m_helm_freq = conf.frequency;

}

void Helm::f_cb_controller_state(
    const mvp_control::ControlProcess::ConstPtr& msg) {
    m_controller_process_values = msg;
}

void Helm::f_iterate() {
    if(m_controller_process_values == nullptr) {
        return;
    }
    /**
     * Acquire state information from finite state machine. Get state name and
     * respective mode to that state.
     */
    auto active_state = m_state_machine->get_active_state();

    auto active_mode = std::find_if(
        m_controller_modes.modes.begin(),
        m_controller_modes.modes.end(),
        [active_state](const mvp_control::ControlMode& mode){
            return mode.name == active_state.mode;
        }
    );

    if(active_mode == std::end(m_controller_modes.modes)) {
        ROS_WARN_STREAM_THROTTLE(10,
            "Active mode '" << active_state.mode << "' can not be found in low"
            " level controller configuration! Helm is skipping.");
        return;
    }
    // Type cast the vector
    std::vector<ctrl::DOF::IDX> dofs;
    std::for_each(
        active_mode->dofs.begin(),
        active_mode->dofs.end(),
        [&](const auto & elem){
            dofs.emplace_back((ctrl::DOF::IDX)elem);
        }
    );

    /**
     * Create holders for priorities and control inputs.
     */
    std::array<double, ctrl::CONTROLLABLE_DOF_LENGTH> dof_ctrl{};
    std::array<int, ctrl::CONTROLLABLE_DOF_LENGTH> dof_priority{};

    for(const auto& i : m_behavior_containers) {

        /**
         * Inform the behavior about the active DOFs
         */
        i->get_behavior()->set_active_dofs(dofs);

        /**
         * Update the system state inside behavior
         */

        i->get_behavior()->register_process_values(*m_controller_process_values);

        /**
         * Request control command from the behavior
         */
        mvp_control::ControlProcess set_point;
        if(!i->get_behavior()->request_set_point(&set_point)) {
            // todo: do something about dysfunctional behavior
            continue;
        }

        /*
         * Check if behavior should be active in active state
         */
        if(!i->get_opts().states.count(active_state.name)) {
            continue;
        }

        /**
         * Get the priority of the behavior in given state
         */
        auto priority = i->get_opts().states[active_state.name];

        /**
         * A behavior might only check for system state and take other actions
         * rather than communicating with the low level controller. Such as
         * drop weight, or cut all the power to the motors etc. In this case,
         * DOFs might not be defined in the behavior. A behavior can do whatever
         * it wants in #BehaviorBase::request_set_point method.
         */
        if(i->get_behavior()->get_dofs().empty()) {
            continue;
        }

        /**
         * Turn requested control command into an array so that it can be
         * processed by iterating degree of freedoms.
         */
        auto bhv_control_array =
            utils::control_process_to_array(set_point);

        for(const auto& dof : i->get_behavior()->get_dofs()) {
            /**
             * This is where the magic happens
             */
            if(priority > dof_priority[dof]) {
                dof_ctrl[dof] = bhv_control_array[dof];
                dof_priority[dof] = priority;
            }
        }

    }

    /**
     * Push commands to low level controller
     */
    auto msg = utils::array_to_control_process_msg(dof_ctrl);

    msg.control_mode = active_state.mode;
    msg.header.stamp = ros::Time::now();
    m_pub_controller_set_point.publish(msg);

}

void Helm::f_helm_loop() {

    ros::Rate r(m_helm_freq);
    while(ros::ok()) {
        f_iterate();
        r.sleep();
    }

}