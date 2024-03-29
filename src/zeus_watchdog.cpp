#include <zeus_watchdog/zeus_watchdog.h>


ZeusWatchdog::ZeusWatchdog(ros::NodeHandle nh, ros::NodeHandle private_nh):
    nh_(nh),
    private_nh_(private_nh)
{
    cmd_vel_sub_ = nh_.subscribe("cmd_vel_in", 1, &ZeusWatchdog::cmdVelCB, this);
    cmd_vel_pub_ = nh_.advertise<geometry_msgs::Twist>("cmd_vel_out", 1);
    status_pub_ = nh_.advertise<std_msgs::Bool>("status", 1);
    info_pub_ = nh_.advertise<zeus_watchdog::TopicArray>("info", 1);
    createTopicMonitors();
}

/*!
   * Fetches the topic information from the parameter server and creates the TopicMonitor objects
   */
bool ZeusWatchdog::createTopicMonitors()
{
    // get how many topics need to be monitored
    bool nb_of_topics_exists = private_nh_.getParam("nb_of_topics", nb_of_topics_);
    if(!nb_of_topics_exists)
    {
        ROS_FATAL("Missing nb_of_topics parameter");
        return false;
    }

    bool rate_exists = private_nh_.getParam("rate", rate_);
    if(!rate_exists)
    {
        ROS_FATAL("Missing rate parameter");
        return false;
    }
    ROS_INFO("Monitoring %d topics", nb_of_topics_);

    // retrieve all topics
    for(int i=0; i<nb_of_topics_; i++)
    {
        std::string topic_id = "topic_" + std::to_string(i+1);
        std::string name, topic_name;
        float min_freq, monitoring_rate;
        bool use_average;
        bool name_exists = private_nh_.getParam( topic_id + "/name" , name);
        bool topic_exists = private_nh_.getParam( topic_id + "/topic_name" , topic_name);
        bool min_freq_exists = private_nh_.getParam( topic_id + "/min_freq" , min_freq);
        bool use_average_exists = private_nh_.getParam( topic_id + "/use_average" , use_average);
        bool monitoring_rate_exists = private_nh_.getParam( topic_id + "/monitoring_rate" , monitoring_rate);
        if(!(name_exists && topic_exists && min_freq_exists && use_average_exists && monitoring_rate_exists))
        {
            ROS_FATAL("One or more parameter for %s is missing", topic_id.c_str());
            return false;
        }
        std::shared_ptr<TopicMonitor> topic = std::make_shared<TopicMonitor>(nh_, private_nh_, name, topic_name, min_freq, use_average, monitoring_rate);
        // TODO: is there a way to subscribe only to the message event instead of receiving the message data?
        topic->start();
        std::cout << topic_id << std::endl;
        topic->printTopicMonitorInfo();
        topic_list_.push_back(topic);
    }
}

/*!
   * Returns the number of topics
   */
int ZeusWatchdog::getNbOfTopics()
{
    return nb_of_topics_;
}

/*!
   * Main loop
   */
void ZeusWatchdog::run()
{
    ros::Rate r(rate_);
    while (ros::ok())
    {
        std_msgs::Bool status_msg;
        zeus_watchdog::TopicArray topic_array_msg;
        status_ = true;

        for(std::shared_ptr<TopicMonitor> t : topic_list_)
        {
            zeus_watchdog::TopicStatus topic_status_msg;
            topic_status_msg.name = t->getName();
            topic_status_msg.status = t->getStatus();
            topic_array_msg.status.push_back(topic_status_msg);
            if(t->getStatus() == false)
                status_ = false;                
        }
        status_msg.data = status_;
        topic_array_msg.header.stamp = ros::Time::now();
        status_pub_.publish(status_msg);
        info_pub_.publish(topic_array_msg);
        r.sleep();
    }
}

void ZeusWatchdog::cmdVelCB(const geometry_msgs::Twist::ConstPtr msg)
{
    if(status_ == true)
    {
        cmd_vel_pub_.publish(msg);
    }
    else
    {
        cmd_vel_pub_.publish(geometry_msgs::Twist());
    }
 }

TopicMonitor::TopicMonitor(ros::NodeHandle nh, ros::NodeHandle private_nh, std::string name, std::string topic_name, 
                           float min_freq, bool use_average, float rate):
    nh_(nh),
    private_nh_(private_nh),
    name_(name),
    topic_name_(topic_name),
    min_freq_(min_freq),
    use_average_(use_average),
    rate_(rate)
{
    min_time_ = 1 / min_freq_;
}

/*!
   * Prints the TopicMonitor info for debugging
   */
void TopicMonitor::printTopicMonitorInfo()
{
    std::cout << "name: " << name_ << std::endl;
    std::cout << "topic_name: " << topic_name_ << std::endl;
    std::cout << "min_freq: " << min_freq_ << std::endl << std::endl;
}

/*!
   * Callback for when a new message is received
   */
void TopicMonitor::topicCB(const ros::MessageEvent<topic_tools::ShapeShifter>& msg)
{
    // TODO: implement option to use message time stamp instead of time received.
    const std::lock_guard<std::mutex> lock(mu_);
    stamps_.push_back(msg.getReceiptTime());
}

/*!
   * Main loop
   */
void TopicMonitor::run()
{
    // Check in the list of time stamps if the time between each message respects the minimum frequency.
    // Once the list is checked, it is cleared and the last time stamp is added at the begining of the
    // new one so that it can also be checked with the next message. The frequency at which the check must
    // run is at most the minimum frequency. For very high minimum frequencies, the check will be run at
    // 10 Hz since there is no reason to go faster than that.
    float run_freq;
    if(min_freq_ < rate_)
        run_freq = min_freq_;
    else
        run_freq = rate_;
    ros::Rate r(run_freq);
    while (ros::ok())
    {
        {
            const std::lock_guard<std::mutex> lock(mu_);
            if(stamps_.size() >= 2)
            {
                status_ = true;
                float elapsed_time_sum = 0;
                for(int i=0; i<stamps_.size()-1; i++)
                {
                    float elapsed_time = (stamps_[i+1] - stamps_[i]).toSec();
                    if(!use_average_ && elapsed_time > min_time_)
                    {
                        status_ = false;
                        break;
                    }
                    else
                        elapsed_time_sum += elapsed_time;
                }
                if(use_average_)
                {
                    float average_time_elapsed = elapsed_time_sum / stamps_.size();
                    // std::cout << "Average time: " << average_time_elapsed << std::endl;
                    if(average_time_elapsed > min_time_)
                    {
                        status_ = false;
                    }
                }
            }
            else
            {
                status_ = false;
            }
            if(stamps_.size() >= 1)
            {
                ros::Time last_stamp = stamps_[stamps_.size()-1];
                stamps_.clear();
                stamps_.push_back(last_stamp);
            }
            // std::cout << "Status: " << status_ <<std::endl;
        }
        r.sleep();
    }
}

/*!
   * Start the topic monitoring thread
   */
void TopicMonitor::start()
{
    sub_ = nh_.subscribe<topic_tools::ShapeShifter>(topic_name_, 1, boost::bind(&TopicMonitor::topicCB, this, _1));
    thread_ = std::thread(&TopicMonitor::run, this);
}

/*!
   * Returns the TopicMonitor status
*/
bool TopicMonitor::getStatus()
{
    const std::lock_guard<std::mutex> lock(mu_);
    return status_;
}

/*!
   * Returns the TopicMonitor name
*/
std::string TopicMonitor::getName()
{
    return name_;
}

int main(int argc, char **argv)
{
    ros::init(argc, argv, "zeus_watchdog");
    ros::NodeHandle nh;
    ros::NodeHandle private_nh("~");
    ZeusWatchdog zeus_watchdog(nh, private_nh);
    ros::AsyncSpinner spinner(zeus_watchdog.getNbOfTopics());
    spinner.start();
    zeus_watchdog.run();
    ros::waitForShutdown();
    return 0;
}