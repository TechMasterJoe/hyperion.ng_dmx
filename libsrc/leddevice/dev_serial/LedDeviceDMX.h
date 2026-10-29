#ifndef LEDEVICEDMX_H
#define LEDEVICEDMX_H

// hyperion includes
#include "ProviderRs232.h"

#include <QJsonObject>
#include <QJsonArray>
#include <vector>

#include <utils/RgbToRgbw.h>

///
/// Custom DMX fixture definition (shared concept with ArtNet)
///
struct DmxFixture
{
	QString name;
	int channelCount = 3;
	std::vector<QString> roles;          // "R","G","B","W","H","L","F"
	std::vector<uint8_t> fixedValues;    // used when role == "F" (or fallback)
};

///
/// Implementation of the LedDevice interface for writing to DMX512 rs232 LED-device.
///
class LedDeviceDMX : public ProviderRs232
{
public:

	///
	/// @brief Constructs a DMX LED-device
	///
	/// @param deviceConfig Device's configuration as JSON-Object
	///
	explicit LedDeviceDMX(const QJsonObject &deviceConfig);

	///
	/// @brief Constructs the LED-device
	///
	/// @param[in] deviceConfig Device's configuration as JSON-Object
	/// @return LedDevice constructed
	static LedDevice* construct(const QJsonObject &deviceConfig);

private:

	///
	/// @brief Initialise the device's configuration
	///
	/// @param[in] deviceConfig the JSON device configuration
	/// @return True, if success
	///
	bool init(const QJsonObject &deviceConfig) override;

	///
	/// @brief Writes the RGB-Color values to the LEDs.
	///
	/// @param[in] ledValues The RGB-color per LED
	/// @return Zero on success, else negative
	///
	int write(const QVector<ColorRgb> &ledValues) override;

	///
	/// @brief Parse custom fixture definitions from config
	///
	bool parseCustomFixtures(const QJsonObject &deviceConfig);

	///
	/// @brief Write one fixture's channels into the DMX buffer
	///
	void writeFixtureChannels(uint8_t* dmxData, int& dmxIdx, const ColorRgb& color, const DmxFixture& fixture);

	int _dmxDeviceType = 0;
	int _dmxStart = 1;
	int _dmxSlotsPerLed = 3;
	int _dmxLedCount = 0;
	unsigned int _dmxChannelCount = 0;

	// Custom fixture support
	bool _useCustomFixtures = false;
	std::vector<DmxFixture> _fixtures;
	std::vector<int> _ledFixtureMap;
	RGBW::WhiteAlgorithm _whiteAlgorithm = RGBW::WhiteAlgorithm::WHITE_OFF;
	ColorRgbw _temp_rgbw{};
};

#endif // LEDEVICEDMX_H
