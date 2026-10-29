#include "LedDeviceUdpArtNet.h"

#ifdef _WIN32
#include <winsock.h>
#else
#include <arpa/inet.h>
#endif

#include <QtEndian>
#include <QHostInfo>
#include <QJsonArray>
#include <QJsonValue>

#include <utils/NetUtils.h>

// Constants
namespace {

	const char CONFIG_HOST[] = "host";
	const char CONFIG_PORT[] = "port";

	const ushort ARTNET_DEFAULT_PORT = 6454;
}

LedDeviceUdpArtNet::LedDeviceUdpArtNet(const QJsonObject &deviceConfig)
	: ProviderUdp(deviceConfig)
	, _whiteAlgorithm(RGBW::WhiteAlgorithm::INVALID)
{
}

LedDevice* LedDeviceUdpArtNet::construct(const QJsonObject &deviceConfig)
{
	return new LedDeviceUdpArtNet(deviceConfig);
}

bool LedDeviceUdpArtNet::parseCustomFixtures(const QJsonObject &deviceConfig)
{
	_fixtures.clear();
	_ledFixtureMap.clear();
	_useCustomFixtures = false;

	if (!deviceConfig.contains("customFixtures") || !deviceConfig["customFixtures"].isArray())
	{
		return true; // not an error, just not using custom fixtures
	}

	QJsonArray fixturesArray = deviceConfig["customFixtures"].toArray();
	if (fixturesArray.isEmpty())
	{
		return true;
	}

	for (const QJsonValue& val : fixturesArray)
	{
		if (!val.isObject())
			continue;

		QJsonObject obj = val.toObject();
		DmxFixture fixture;
		fixture.name = obj["name"].toString("Unnamed");
		fixture.channelCount = obj["channelCount"].toInt(3);
		if (fixture.channelCount < 1 || fixture.channelCount > 512)
			fixture.channelCount = 3;

		// Prefer explicit roles array
		if (obj.contains("channelRoles") && obj["channelRoles"].isArray())
		{
			QJsonArray roles = obj["channelRoles"].toArray();
			for (const QJsonValue& r : roles)
			{
				fixture.roles.push_back(r.toString().toUpper());
			}
		}
		// Fallback: compact string like "RGBH"
		else if (obj.contains("channelConfig"))
		{
			QString cfg = obj["channelConfig"].toString().toUpper();
			for (QChar c : cfg)
			{
				fixture.roles.push_back(QString(c));
			}
			fixture.channelCount = static_cast<int>(fixture.roles.size());
		}

		// Ensure roles vector matches channelCount
		while (static_cast<int>(fixture.roles.size()) < fixture.channelCount)
		{
			fixture.roles.push_back("L");
		}
		if (static_cast<int>(fixture.roles.size()) > fixture.channelCount)
		{
			fixture.roles.resize(fixture.channelCount);
		}

		// Fixed values
		if (obj.contains("fixedValues") && obj["fixedValues"].isArray())
		{
			QJsonArray fixed = obj["fixedValues"].toArray();
			for (const QJsonValue& f : fixed)
			{
				fixture.fixedValues.push_back(static_cast<uint8_t>(qBound(0, f.toInt(0), 255)));
			}
		}
		while (fixture.fixedValues.size() < static_cast<size_t>(fixture.channelCount))
		{
			fixture.fixedValues.push_back(0);
		}

		_fixtures.push_back(fixture);
		Debug(_log, "Loaded custom fixture '%s' with %d channels", QSTRING_CSTR(fixture.name), fixture.channelCount);
	}

	if (_fixtures.empty())
	{
		return true;
	}

	_useCustomFixtures = true;

	// Optional per-LED fixture index map
	if (deviceConfig.contains("ledFixtureMap") && deviceConfig["ledFixtureMap"].isArray())
	{
		QJsonArray map = deviceConfig["ledFixtureMap"].toArray();
		for (const QJsonValue& m : map)
		{
			int idx = m.toInt(0);
			if (idx < 0 || idx >= static_cast<int>(_fixtures.size()))
				idx = 0;
			_ledFixtureMap.push_back(idx);
		}
	}

	Info(_log, "Custom DMX fixtures enabled: %d fixture type(s)", static_cast<int>(_fixtures.size()));
	return true;
}

bool LedDeviceUdpArtNet::init(const QJsonObject &deviceConfig)
{
	// Initialise sub-class
	if (!ProviderUdp::init(deviceConfig))
	{
		return false;
	}

	_hostName = _devConfig[CONFIG_HOST].toString();
	_port = deviceConfig[CONFIG_PORT].toInt(ARTNET_DEFAULT_PORT);

	// Initialize white algorithm
	QString whiteAlgorithmStr = deviceConfig["whiteAlgorithm"].toString("white_off");
	_whiteAlgorithm = RGBW::stringToWhiteAlgorithm(whiteAlgorithmStr);
	if (_whiteAlgorithm == RGBW::WhiteAlgorithm::INVALID)
	{
		QString errortext = QString("Unknown White-Algorithm: %1").arg(whiteAlgorithmStr);
		this->setInError(errortext);
		return false;
	}
	Debug(_log, "White-Algorithm   : %s", QSTRING_CSTR(whiteAlgorithmStr));
	_ledChannelsPerFixture = (_whiteAlgorithm == RGBW::WhiteAlgorithm::WHITE_OFF) ? 3 : 4;

	_artnet_universe = deviceConfig["universe"].toInt(1);
	_artnet_channelsPerFixture = deviceConfig["channelsPerFixture"].toInt(_ledChannelsPerFixture);

	// Validate channelsPerFixture
	if (_artnet_channelsPerFixture < _ledChannelsPerFixture)
	{
		Error(_log, "channelsPerFixture must be at least %d. Setting to %d.", _ledChannelsPerFixture, _ledChannelsPerFixture);
		_artnet_channelsPerFixture = _ledChannelsPerFixture;
	}

	// Parse optional custom fixtures (does not fail init if absent)
	if (!parseCustomFixtures(deviceConfig))
	{
		return false;
	}

	return true;
}

int LedDeviceUdpArtNet::open()
{
	_isDeviceReady = false;
	this->setIsRecoverable(true);

	NetUtils::convertMdnsToIp(_log, _hostName);
	if (ProviderUdp::open() == 0)
	{
		// Everything is OK, device is ready
		_isDeviceReady = true;

	}
	return _isDeviceReady ? 0 : -1;
}

// populates the headers
void LedDeviceUdpArtNet::prepare(unsigned this_universe, unsigned this_sequence, unsigned this_dmxChannelCount)
{
	// WTF? why do the specs say:
	// "This value should be an even number in the range 2 – 512. "
	if (this_dmxChannelCount & 0x1)
	{
		this_dmxChannelCount++;
	}

	memcpy(artnet_packet.header.ID, "Art-Net\0", 8);
	artnet_packet.header.OpCode = qToLittleEndian<quint16>(0x5000);
	artnet_packet.header.ProtVer = qToBigEndian<quint16>(14);
	artnet_packet.header.Sequence = static_cast<quint8>(this_sequence);
	artnet_packet.header.Physical = 0;
	artnet_packet.header.SubUni = this_universe & 0xFF;
	artnet_packet.header.Net = (this_universe >> 8) & 0x7F;
	artnet_packet.header.Length = qToBigEndian<quint16>(static_cast<quint16>(this_dmxChannelCount));

}

void LedDeviceUdpArtNet::writeFixtureChannels(uint8_t* dmxData, int& dmxIdx, const ColorRgb& color, const DmxFixture& fixture)
{
	ColorRgbw rgbw;
	bool hasWhite = false;
	for (const QString& role : fixture.roles)
	{
		if (role == "W")
		{
			hasWhite = true;
			break;
		}
	}

	if (hasWhite && _whiteAlgorithm != RGBW::WhiteAlgorithm::WHITE_OFF)
	{
		RGBW::Rgb_to_Rgbw(color, &rgbw, _whiteAlgorithm);
	}
	else
	{
		rgbw.red = color.red;
		rgbw.green = color.green;
		rgbw.blue = color.blue;
		rgbw.white = 0;
	}

	for (int i = 0; i < fixture.channelCount && dmxIdx < DMX_MAX; ++i)
	{
		const QString& role = fixture.roles[static_cast<size_t>(i)];
		uint8_t value = 0;

		if (role == "R")
			value = rgbw.red;
		else if (role == "G")
			value = rgbw.green;
		else if (role == "B")
			value = rgbw.blue;
		else if (role == "W")
			value = rgbw.white;
		else if (role == "H")
			value = 255;
		else if (role == "L")
			value = 0;
		else if (role == "F" && i < static_cast<int>(fixture.fixedValues.size()))
			value = fixture.fixedValues[static_cast<size_t>(i)];
		else
			value = (i < static_cast<int>(fixture.fixedValues.size())) ? fixture.fixedValues[static_cast<size_t>(i)] : 0;

		dmxData[dmxIdx++] = value;
	}
}

int LedDeviceUdpArtNet::write(const QVector<ColorRgb> &ledValues)
{
	int retVal = 0;
	int thisUniverse = _artnet_universe;

	/*
	This field is incremented in the range 0x01 to 0xff to allow the receiving node to resequence packets.
	The Sequence field is set to 0x00 to disable this feature.
	*/
	if (_artnet_seq++ == 0)
	{
		_artnet_seq = 1;
	}

	int dmxIdx = 0; // offset into the current dmx packet

	memset(artnet_packet.raw, 0, sizeof(artnet_packet.raw));

	int ledIndex = 0;
	for (const ColorRgb& color : ledValues)
	{
		int channelsNeeded = _artnet_channelsPerFixture;
		const DmxFixture* fixture = nullptr;

		if (_useCustomFixtures && !_fixtures.empty())
		{
			int fixtureIdx = 0;
			if (ledIndex < static_cast<int>(_ledFixtureMap.size()))
				fixtureIdx = _ledFixtureMap[static_cast<size_t>(ledIndex)];
			else if (!_ledFixtureMap.empty())
				fixtureIdx = _ledFixtureMap.back();
			// else default to fixture 0

			if (fixtureIdx >= 0 && fixtureIdx < static_cast<int>(_fixtures.size()))
			{
				fixture = &_fixtures[static_cast<size_t>(fixtureIdx)];
				channelsNeeded = fixture->channelCount;
			}
		}

		// Check if this LED would overflow the DMX packet
		if (dmxIdx + channelsNeeded > DMX_MAX)
		{
			// Send current packet before continuing
			prepare(thisUniverse, _artnet_seq, dmxIdx);

			if (writeBytes(18 + dmxIdx, artnet_packet.raw) < 0)
			{
				retVal = -1;
			}

			memset(artnet_packet.raw, 0, sizeof(artnet_packet.raw));
			thisUniverse++;
			dmxIdx = 0;
		}

		if (fixture != nullptr)
		{
			writeFixtureChannels(artnet_packet.Data, dmxIdx, color, *fixture);
		}
		else
		{
			// Original behavior
			if (_whiteAlgorithm == RGBW::WhiteAlgorithm::WHITE_OFF)
			{
				artnet_packet.Data[dmxIdx++] = color.red;
				artnet_packet.Data[dmxIdx++] = color.green;
				artnet_packet.Data[dmxIdx++] = color.blue;
			}
			else
			{
				RGBW::Rgb_to_Rgbw(color, &_temp_rgbw, _whiteAlgorithm);
				artnet_packet.Data[dmxIdx++] = _temp_rgbw.red;
				artnet_packet.Data[dmxIdx++] = _temp_rgbw.green;
				artnet_packet.Data[dmxIdx++] = _temp_rgbw.blue;
				artnet_packet.Data[dmxIdx++] = _temp_rgbw.white;
			}

			// Skip extra channels if fixture needs more than RGB/RGBW
			dmxIdx += (_artnet_channelsPerFixture - _ledChannelsPerFixture);
		}

		ledIndex++;
	}

	// Send the last packet if there's any data
	if (dmxIdx > 0)
	{
		prepare(thisUniverse, _artnet_seq, dmxIdx);

		if (writeBytes(18 + dmxIdx, artnet_packet.raw) < 0)
		{
			retVal = -1;
		}
	}

	return retVal;
}
