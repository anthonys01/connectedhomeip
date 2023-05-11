#include "mqtt_listener.h"

#include <iostream>

MqttListener::MqttListener(const std::string& broker_uri, const std::string& client_id)
  : m_client(broker_uri, client_id)
{
  m_connect_options.set_keep_alive_interval(20);
  m_connect_options.set_clean_session(true);

  m_client.set_callback(*this);
}

void MqttListener::connection_lost(const std::string& cause)
{
  std::cout << "Connection lost: " << cause << std::endl;
}

void MqttListener::message_arrived(mqtt::const_message_ptr msg)
{
  std::cout << "Message arrived: " << msg->to_string() << std::endl;
  m_last_message = msg;
  if (message_callback_) {
    message_callback_(msg->get_topic(), msg->to_string());
  }
}

void MqttListener::delivery_complete(mqtt::delivery_token_ptr token)
{
  std::cout << "Delivery complete for token: " << token->get_message_id() << std::endl;
}

void MqttListener::on_failure(const mqtt::token& tok)
{
  std::cout << "Connection attempt failed" << std::endl;
}

void MqttListener::on_success(const mqtt::token& tok)
{
  std::cout << "Connection attempt successful" << std::endl;
}

void MqttListener::start()
{
  try {
    m_client.connect(m_connect_options, nullptr, *this)->wait();
  }
  catch (const mqtt::exception& exc) {
    std::cerr << "Failed to start MQTT listener: " << exc.what() << std::endl;
  }
}

void MqttListener::stop()
{
  try {
    m_client.disconnect()->wait();
  }
  catch (const mqtt::exception& exc) {
    std::cerr << "Failed to stop MQTT listener: " << exc.what() << std::endl;
  }
}

void MqttListener::addTopic(const std::string& topic) {
  topics_.push_back(topic);

  if (m_client.is_connected()) {
    try {
      m_client.subscribe(topic, 0)->wait();
      std::cout << "Subscribed to topic: " << topic << std::endl;
    } catch (const mqtt::exception& ex) {
      std::cerr << "Error subscribing to topic: " << ex.what() << std::endl;
    }
  } else {
    std::cerr << "cannot add topic " << topic << " because not connected " << std::endl;
  }
}

void MqttListener::removeTopic(const std::string& topic) {
  for (auto it = topics_.begin(); it != topics_.end(); ++it) {
    if (*it == topic) {
      it = topics_.erase(it);
      break;
    }
  }

  if (m_client.is_connected()) {
    try {
      m_client.unsubscribe(topic)->wait();
      std::cout << "Unsubscribed from topic: " << topic << std::endl;
    } catch (const mqtt::exception& ex) {
      std::cerr << "Error unsubscribing from topic: " << ex.what() << std::endl;
    }
  }
}

void MqttListener::publish(const std::string& topic, const std::string& payload, bool retained) {
    if (!m_client.is_connected()) {
        std::cerr << "Error: MQTT client is not connected." << std::endl;
        return;
    }

    auto message = mqtt::make_message(topic, payload);
    message->set_qos(1);
    message->set_retained(retained);
    m_client.publish(message);
}

void MqttListener::set_message_callback(std::function<void(const std::string&, const std::string&)> message_callback) {
    message_callback_ = std::move(message_callback);
}