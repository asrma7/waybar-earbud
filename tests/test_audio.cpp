#include <cassert>
#include <optional>
#include <string>

#include "../src/audio.hpp"

int main() {
    {
        auto mac = audio::mac_from_bluez_token("bluez_output.EC_73_79_55_D7_E7.1");
        assert(mac);
        assert(*mac == "EC:73:79:55:D7:E7");
    }

    {
        auto mac = audio::mac_from_bluez_token("bluez_output.80_99_E7_FD_1F_7C.a2dp-sink");
        assert(mac);
        assert(*mac == "80:99:E7:FD:1F:7C");
    }

    {
        auto mac = audio::mac_from_bluez_token("alsa_output.pci-0000_00_1f.3.analog-stereo");
        assert(!mac);
    }

    {
        std::string default_sink = "bluez_output.EC_73_79_55_D7_E7.1";
        std::string sinks = R"(Sink #42
	State: RUNNING
	Name: bluez_output.EC_73_79_55_D7_E7.1
	Properties:
		bluez.path = "/org/bluez/hci0/dev_EC_73_79_55_D7_E7"
)";
        auto mac = audio::mac_from_pactl_sinks(default_sink, sinks);
        assert(mac);
        assert(*mac == "EC:73:79:55:D7:E7");
    }

    {
        std::string default_sink = "alsa_output.pci-0000_00_1f.3.analog-stereo";
        std::string sinks = R"(Sink #1
	Name: alsa_output.pci-0000_00_1f.3.analog-stereo
	Properties:
		device.description = "Built-in Audio"
Sink #2
	Name: bluez_output.EC_73_79_55_D7_E7.1
)";
        auto mac = audio::mac_from_pactl_sinks(default_sink, sinks);
        assert(!mac);
    }

    return 0;
}
