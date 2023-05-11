#include "mqtt_listener.h"

int main(int argc, char* argv[]) {
  const std::string broker_uri = "tcp://localhost:1883";
  const std::string client_id = "mqtt_listener";
  const std::string topic = "datetime";

  MqttListener listener(broker_uri, client_id);
  listener.start();
  listener.addTopic(topic);

  int input;

  // Wait for messages to arrive (indefinitely)
  while (true) {
    std::cout << "Press Enter to send a MQTT message or Ctrl+D to exit..." << std::endl;
        input = std::cin.get(); // waits for the user to press a key

        if (input == '\n') { // Enter key
            // perform the desired action here
            listener.publish("test", "test message", false);
            std::cout << "MQTT message sent on topic 'test' !" << std::endl;
        }
        else if (std::cin.eof()) { // Ctrl+D
            break;
        }
  }

  listener.stop();

  return 0;
}