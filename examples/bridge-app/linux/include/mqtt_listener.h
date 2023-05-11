#ifndef MQTT_LISTENER_H
#define MQTT_LISTENER_H

#include "mqtt/async_client.h"
#include <functional>

class MqttListener : public virtual mqtt::callback,
                     public virtual mqtt::iaction_listener {
public:
  MqttListener(const std::string& broker_uri, const std::string& client_id);

  virtual void connection_lost(const std::string& cause) override;
  virtual void message_arrived(mqtt::const_message_ptr msg) override;
  virtual void delivery_complete(mqtt::delivery_token_ptr token) override;
  virtual void on_failure(const mqtt::token& tok) override;
  virtual void on_success(const mqtt::token& tok) override;

  void start();
  void stop();
  void addTopic(const std::string& topic);
  void removeTopic(const std::string& topic);
  void publish(const std::string& topic, const std::string& payload, bool retained);
  void set_message_callback(std::function<void(const std::string&, const std::string&)> message_callback);

private:
  mqtt::async_client m_client;
  mqtt::connect_options m_connect_options;
  mqtt::const_message_ptr m_last_message;
  std::vector<std::string> topics_;
  std::function<void(const std::string&, const std::string&)> message_callback_;
};

#endif /* MQTT_LISTENER_H */