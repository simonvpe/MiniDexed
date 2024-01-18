//
// mididevice.cpp
//
// MiniDexed - Dexed FM synthesizer for bare metal Raspberry Pi
// Copyright (C) 2022  The MiniDexed Team
//
// Original author of this class:
//	R. Stange <rsta2@o2online.de>
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.
//

#include <circle/logger.h>
#include "mididevice.h"
#include "minidexed.h"
#include "config.h"
#include <stdio.h>
#include <assert.h>
#include "userinterface.h"

LOGMODULE ("mididevice");

#define MIDI_NOTE_OFF		0b1000
#define MIDI_NOTE_ON		0b1001
#define MIDI_AFTERTOUCH		0b1010			// TODO
#define MIDI_CHANNEL_AFTERTOUCH 0b1101   // right now Synth_Dexed just manage Channel Aftertouch not Polyphonic AT -> 0b1010
#define MIDI_CONTROL_CHANGE	0b1011
	#define MIDI_CC_BANK_SELECT_MSB		0
	#define MIDI_CC_MODULATION			1
	#define MIDI_CC_BREATH_CONTROLLER	2 
	#define MIDI_CC_FOOT_PEDAL 		4
	#define MIDI_CC_VOLUME				7
	#define MIDI_CC_PAN_POSITION		10
	#define MIDI_CC_BANK_SELECT_LSB		32
	#define MIDI_CC_BANK_SUSTAIN		64
	#define MIDI_CC_RESONANCE			71
	#define MIDI_CC_FREQUENCY_CUTOFF	74
	#define MIDI_CC_REVERB_LEVEL		91
	#define MIDI_CC_DETUNE_LEVEL		94
	#define MIDI_CC_ALL_SOUND_OFF		120
	#define MIDI_CC_ALL_NOTES_OFF		123
	#define MIDI_CC_NRPN_PARAM_MSB		99
	#define MIDI_CC_NRPN_PARAM_LSB		98
	#define MIDI_CC_NRPN_DATA_LSB		38
#define MIDI_PROGRAM_CHANGE	0b1100
#define MIDI_PITCH_BEND		0b1110

#define MIDI_SYSTEM_EXCLUSIVE_BEGIN	0xF0
#define MIDI_SYSTEM_EXCLUSIVE_END	0xF7
#define MIDI_TIMING_CLOCK	0xF8
#define MIDI_ACTIVE_SENSING	0xFE

#define MIDI_NRPN_PROGRAM_CHANGE	21

uint8_t scale(uint8_t max, uint8_t value) {
	return (uint8_t)((((unsigned)value) * max) / 127);
}

CMIDIDevice::TDeviceMap CMIDIDevice::s_DeviceMap;

CMIDIDevice::CMIDIDevice (CMiniDexed *pSynthesizer, CConfig *pConfig, CUserInterface *pUI)
:	m_pSynthesizer (pSynthesizer),
	m_pConfig (pConfig),
	m_pUI (pUI)
{
	for (unsigned nTG = 0; nTG < CConfig::ToneGenerators; nTG++)
	{
		m_ChannelMap[nTG] = Disabled;
		m_NRPNoffset[nTG] = 0;
		m_NRPNop[nTG] = 0;
	}
}

CMIDIDevice::~CMIDIDevice (void)
{
	m_pSynthesizer = 0;
}

void CMIDIDevice::SetChannel (u8 ucChannel, unsigned nTG)
{
	assert (nTG < CConfig::ToneGenerators);
	m_ChannelMap[nTG] = ucChannel;
}

u8 CMIDIDevice::GetChannel (unsigned nTG) const
{
	assert (nTG < CConfig::ToneGenerators);
	return m_ChannelMap[nTG];
}

void CMIDIDevice::MIDIMessageHandler (const u8 *pMessage, size_t nLength, unsigned nCable)
{
	// The packet contents are just normal MIDI data - see
	// https://www.midi.org/specifications/item/table-1-summary-of-midi-message

	if (m_pConfig->GetMIDIDumpEnabled ())
	{
		switch (nLength)
		{
		case 1:
			if (   pMessage[0] != MIDI_TIMING_CLOCK
			    && pMessage[0] != MIDI_ACTIVE_SENSING)
			{
				printf ("MIDI%u: %02X\n", nCable, (unsigned) pMessage[0]);
			}
			break;

		case 2:
			printf ("MIDI%u: %02X %02X\n", nCable,
				(unsigned) pMessage[0], (unsigned) pMessage[1]);
			break;

		case 3:
			printf ("MIDI%u: %02X %02X %02X\n", nCable,
				(unsigned) pMessage[0], (unsigned) pMessage[1],
				(unsigned) pMessage[2]);
			break;
		default:
			switch(pMessage[0])
			{
				case MIDI_SYSTEM_EXCLUSIVE_BEGIN:
					printf("MIDI%u: SysEx data length: [%d]:",nCable, uint16_t(nLength));
					for (uint16_t i = 0; i < nLength; i++)
					{
						if((i % 16) == 0)
							printf("\n%04d:",i);
						printf(" 0x%02x",pMessage[i]);
					}
					printf("\n");
					break;
				default:
					printf("MIDI%u: Unhandled MIDI event type %0x02x\n",nCable,pMessage[0]);
			}
			break;
		}
	}

	// Only for debugging:
/*
	if(pMessage[0]==MIDI_SYSTEM_EXCLUSIVE_BEGIN)
	{
		printf("MIDI%u: SysEx data length: [%d]:",nCable, uint16_t(nLength));
		for (uint16_t i = 0; i < nLength; i++)
		{
			if((i % 16) == 0)
				printf("\n%04d:",i);
			printf(" 0x%02x",pMessage[i]);
		}
		printf("\n");
	}
*/

	// Handle MIDI Thru
	if (m_DeviceName.compare (m_pConfig->GetMIDIThruIn ()) == 0)
	{
		TDeviceMap::const_iterator Iterator;

		Iterator = s_DeviceMap.find (m_pConfig->GetMIDIThruOut ());
		if (Iterator != s_DeviceMap.end ())
		{
			Iterator->second->Send (pMessage, nLength, nCable);
		}
	}

	if (nLength < 2)
	{
		// LOGERR("MIDI message is shorter than 2 bytes!");
		return;
	}

	m_MIDISpinLock.Acquire ();

	u8 ucStatus  = pMessage[0];
	u8 ucChannel = ucStatus & 0x0F;
	u8 ucType    = ucStatus >> 4;

	// GLOBAL MIDI SYSEX
	if (pMessage[0] == MIDI_SYSTEM_EXCLUSIVE_BEGIN && pMessage[3] == 0x04 &&  pMessage[4] == 0x01 && pMessage[nLength-1] == MIDI_SYSTEM_EXCLUSIVE_END) // MASTER VOLUME
	{
		float32_t nMasterVolume=((pMessage[5] & 0x7c) & ((pMessage[6] & 0x7c) <<7))/(1<<14);
		LOGNOTE("Master volume: %f",nMasterVolume);
		m_pSynthesizer->setMasterVolume(nMasterVolume);
	}
	else
	{
		// Perform any MiniDexed level MIDI handling before specific Tone Generators
		switch (ucType)
		{
		case MIDI_CONTROL_CHANGE:
		case MIDI_NOTE_OFF:
		case MIDI_NOTE_ON:
			if (nLength < 3)
			{
				break;
			}
			m_pUI->UIMIDICmdHandler (ucChannel, ucStatus & 0xF0, pMessage[1], pMessage[2]);
			break;
		case MIDI_PROGRAM_CHANGE:
			// Check for performance PC messages
			if( m_pConfig->GetMIDIRXProgramChange() )
			{
				unsigned nPerfCh = m_pSynthesizer->GetPerformanceSelectChannel();
				if( nPerfCh != Disabled)
				{
					if ((ucChannel == nPerfCh) || (nPerfCh == OmniMode))
					{
						//printf("Performance Select Channel %d\n", nPerfCh);
						m_pSynthesizer->ProgramChangePerformance (pMessage[1]);
					}
				}
			}
			break;
		}

		// Process MIDI for each Tone Generator
		for (unsigned nTG = 0; nTG < CConfig::ToneGenerators; nTG++)
		{
			if (ucStatus == MIDI_SYSTEM_EXCLUSIVE_BEGIN)
			{
				// MIDI SYSEX per MIDI channel
				uint8_t ucSysExChannel = (pMessage[2] & 0x0F);
				if (m_ChannelMap[nTG] == ucSysExChannel || m_ChannelMap[nTG] == OmniMode)
				{
					LOGNOTE("MIDI-SYSEX: channel: %u, len: %u, TG: %u",m_ChannelMap[nTG],nLength,nTG);
					HandleSystemExclusive(pMessage, nLength, nCable, nTG);
				}
			}
			else
			{
				if (   m_ChannelMap[nTG] == ucChannel
				    || m_ChannelMap[nTG] == OmniMode)
				{
					switch (ucType)
					{
					case MIDI_NOTE_ON:
						if (nLength < 3)
						{
							break;
						}
		
						if (pMessage[2] > 0)
						{
							if (pMessage[2] <= 127)
							{
								m_pSynthesizer->keydown (pMessage[1],
											 pMessage[2], nTG);
							}
						}
						else
						{
							m_pSynthesizer->keyup (pMessage[1], nTG);
						}
						break;
		
					case MIDI_NOTE_OFF:
						if (nLength < 3)
						{
							break;
						}
		
						m_pSynthesizer->keyup (pMessage[1], nTG);
						break;
		
					case MIDI_CHANNEL_AFTERTOUCH:
						
						m_pSynthesizer->setAftertouch (pMessage[1], nTG);
						m_pSynthesizer->ControllersRefresh (nTG);
						break;
							
					case MIDI_CONTROL_CHANGE:
						if (nLength < 3)
						{
							break;
						}
		
						switch (pMessage[1])
						{
						case MIDI_CC_MODULATION:
							m_pSynthesizer->setModWheel (pMessage[2], nTG);
							m_pSynthesizer->ControllersRefresh (nTG);
							break;
								
						case MIDI_CC_FOOT_PEDAL:
							m_pSynthesizer->setFootController (pMessage[2], nTG);
							m_pSynthesizer->ControllersRefresh (nTG);
							break;

						case MIDI_CC_BREATH_CONTROLLER:
							m_pSynthesizer->setBreathController (pMessage[2], nTG);
							m_pSynthesizer->ControllersRefresh (nTG);
							break;
								
						case MIDI_CC_VOLUME:
							m_pSynthesizer->SetVolume (pMessage[2], nTG);
							break;
		
						case MIDI_CC_PAN_POSITION:
							m_pSynthesizer->SetPan (pMessage[2], nTG);
							break;
		
						case MIDI_CC_BANK_SELECT_MSB:
							m_pSynthesizer->BankSelectMSB (pMessage[2], nTG);
							break;
		
						case MIDI_CC_BANK_SELECT_LSB:
							m_pSynthesizer->BankSelectLSB (pMessage[2], nTG);
							break;
		
						case MIDI_CC_BANK_SUSTAIN:
							m_pSynthesizer->setSustain (pMessage[2] >= 64, nTG);
							break;
		
						case MIDI_CC_RESONANCE:
							m_pSynthesizer->SetResonance (maplong (pMessage[2], 0, 127, 0, 99), nTG);
							break;
							
						case MIDI_CC_FREQUENCY_CUTOFF:
							m_pSynthesizer->SetCutoff (maplong (pMessage[2], 0, 127, 0, 99), nTG);
							break;
		
						case MIDI_CC_REVERB_LEVEL:
							m_pSynthesizer->SetReverbSend (maplong (pMessage[2], 0, 127, 0, 99), nTG);
							break;
		
						case MIDI_CC_DETUNE_LEVEL:
							if (pMessage[2] == 0)
							{
								// "0 to 127, with 0 being no celeste (detune) effect applied at all."
								m_pSynthesizer->SetMasterTune (0, nTG);
							}
							else
							{
								m_pSynthesizer->SetMasterTune (maplong (pMessage[2], 1, 127, -99, 99), nTG);
							}
							break;
		
						case MIDI_CC_ALL_SOUND_OFF:
							m_pSynthesizer->panic (pMessage[2], nTG);
							break;
		
						case MIDI_CC_ALL_NOTES_OFF:
							// As per "MIDI 1.0 Detailed Specification" v4.2
							// From "ALL NOTES OFF" states:
							// "Receivers should ignore an All Notes Off message while Omni is on (Modes 1 & 2)"
							if (!m_pConfig->GetIgnoreAllNotesOff () && m_ChannelMap[nTG] != OmniMode)
							{
								m_pSynthesizer->notesOff (pMessage[2], nTG);
							}
							break;

						case MIDI_CC_NRPN_PARAM_MSB:
							if(pMessage[2] <= 6) {
								m_NRPNop[nTG] = pMessage[2];
							}
							break;

						case MIDI_CC_NRPN_PARAM_LSB:
							m_NRPNoffset[nTG] = pMessage[2];
							break;

						case MIDI_CC_NRPN_DATA_LSB:
							switch(m_NRPNoffset[nTG])
							{
							case MIDI_NRPN_PROGRAM_CHANGE:
								m_pSynthesizer->ProgramChange(pMessage[2], nTG);
								break;
							default:
								if(m_NRPNop[nTG] < 6)
								{
									switch(m_NRPNoffset[nTG]) {
										case DEXED_OP_EG_R1:
											m_pSynthesizer->m_pTG[nTG]->setOPRate(m_NRPNop[nTG], 0, scale(99, pMessage[2]));
											break;
										case DEXED_OP_EG_R2:
											m_pSynthesizer->m_pTG[nTG]->setOPRate(m_NRPNop[nTG], 1, scale(99, pMessage[2]));
											break;
										case DEXED_OP_EG_R3:
											m_pSynthesizer->m_pTG[nTG]->setOPRate(m_NRPNop[nTG], 2, scale(99, pMessage[2]));
											break;
										case DEXED_OP_EG_R4:
											m_pSynthesizer->m_pTG[nTG]->setOPRate(m_NRPNop[nTG], 3, scale(99, pMessage[2]));
											break;
										case DEXED_OP_EG_L1:
											m_pSynthesizer->m_pTG[nTG]->setOPLevel(m_NRPNop[nTG], 0, scale(99, pMessage[2]));
											break;
										case DEXED_OP_EG_L2:
											m_pSynthesizer->m_pTG[nTG]->setOPLevel(m_NRPNop[nTG], 1, scale(99, pMessage[2]));
											break;
										case DEXED_OP_EG_L3:
											m_pSynthesizer->m_pTG[nTG]->setOPLevel(m_NRPNop[nTG], 2, scale(99, pMessage[2]));
											break;
										case DEXED_OP_EG_L4:
											m_pSynthesizer->m_pTG[nTG]->setOPLevel(m_NRPNop[nTG], 3, scale(99, pMessage[2]));
											break;
										case DEXED_OP_LEV_SCL_BRK_PT:
											m_pSynthesizer->m_pTG[nTG]->setOPKeyboardLevelScalingBreakPoint(m_NRPNop[nTG], scale(99, pMessage[2]));
											break;
										case DEXED_OP_SCL_LEFT_DEPTH:
											m_pSynthesizer->m_pTG[nTG]->setOPKeyboardLevelScalingDepthLeft(m_NRPNop[nTG], scale(99, pMessage[2]));
											break;
										case DEXED_OP_SCL_RGHT_DEPTH:
											m_pSynthesizer->m_pTG[nTG]->setOPKeyboardLevelScalingDepthRight(m_NRPNop[nTG], scale(99, pMessage[2]));
											break;
										case DEXED_OP_SCL_LEFT_CURVE:
											m_pSynthesizer->m_pTG[nTG]->setOPKeyboardLevelScalingCurveLeft(m_NRPNop[nTG], scale(3, pMessage[2]));
											break;
										case DEXED_OP_SCL_RGHT_CURVE:
											m_pSynthesizer->m_pTG[nTG]->setOPKeyboardLevelScalingCurveRight(m_NRPNop[nTG], scale(3, pMessage[2]));
											break;
										case DEXED_OP_OSC_RATE_SCALE:
											m_pSynthesizer->m_pTG[nTG]->setOPKeyboardRateScale(m_NRPNop[nTG], scale(7, pMessage[2]));
											break;
										case DEXED_OP_AMP_MOD_SENS:
											m_pSynthesizer->m_pTG[nTG]->setOPAmpModulationSensity(m_NRPNop[nTG], scale(3, pMessage[2]));
											break;
										case DEXED_OP_KEY_VEL_SENS:
											m_pSynthesizer->m_pTG[nTG]->setOPKeyboardVelocitySensity(m_NRPNop[nTG], scale(7, pMessage[2]));
											break;
										case DEXED_OP_OUTPUT_LEV:
											m_pSynthesizer->m_pTG[nTG]->setOPOutputLevel(m_NRPNop[nTG], scale(99, pMessage[2]));
											break;
										case DEXED_OP_OSC_MODE:
											m_pSynthesizer->m_pTG[nTG]->setOPMode(m_NRPNop[nTG], scale(1, pMessage[2]));
											break;
										case DEXED_OP_FREQ_COARSE:
											m_pSynthesizer->m_pTG[nTG]->setOPFrequencyCoarse(m_NRPNop[nTG], scale(31, pMessage[2]));
											break;
										case DEXED_OP_FREQ_FINE:
											m_pSynthesizer->m_pTG[nTG]->setOPFrequencyFine(m_NRPNop[nTG], scale(99, pMessage[2]));
											break;
										case DEXED_OP_OSC_DETUNE:
											m_pSynthesizer->m_pTG[nTG]->setOPDetune(m_NRPNop[nTG], scale(14, pMessage[2]));
											break;
									}
									// TODO: send partial update, but this will do for now
									SendSystemExclusiveVoice(0, 0, nTG);
								}
								if(m_NRPNop[nTG] == 6) {
									switch(m_NRPNoffset[nTG]) {
										case  DEXED_PITCH_EG_R1:
											m_pSynthesizer->m_pTG[nTG]->setPitchRate(0, scale(99, pMessage[2]));
											break;
										case  DEXED_PITCH_EG_R2:
											m_pSynthesizer->m_pTG[nTG]->setPitchRate(1, scale(99, pMessage[2]));

											break;
										case  DEXED_PITCH_EG_R3:
											m_pSynthesizer->m_pTG[nTG]->setPitchRate(2, scale(99, pMessage[2]));

											break;
										case  DEXED_PITCH_EG_R4:
											m_pSynthesizer->m_pTG[nTG]->setPitchRate(3, scale(99, pMessage[2]));

											break;
										case  DEXED_PITCH_EG_L1:
											m_pSynthesizer->m_pTG[nTG]->setPitchLevel(0, scale(99, pMessage[2]));

											break;
										case  DEXED_PITCH_EG_L2:
											m_pSynthesizer->m_pTG[nTG]->setPitchLevel(1, scale(99, pMessage[2]));

											break;
										case  DEXED_PITCH_EG_L3:
											m_pSynthesizer->m_pTG[nTG]->setPitchLevel(2, scale(99, pMessage[2]));

											break;
										case  DEXED_PITCH_EG_L4:
											m_pSynthesizer->m_pTG[nTG]->setPitchLevel(3, scale(99, pMessage[2]));

											break;
										case  DEXED_ALGORITHM:
											m_pSynthesizer->m_pTG[nTG]->setAlgorithm(scale(31, pMessage[2]));

											break;
										case  DEXED_FEEDBACK:
											m_pSynthesizer->m_pTG[nTG]->setFeedback(scale(7, pMessage[2]));

											break;
										case  DEXED_OSC_KEY_SYNC:
											m_pSynthesizer->m_pTG[nTG]->setOscillatorSync(scale(1, pMessage[2]));

											break;
										case  DEXED_LFO_SPEED:
											m_pSynthesizer->m_pTG[nTG]->setLFOSpeed(scale(99, pMessage[2]));

											break;
										case  DEXED_LFO_DELAY:
											m_pSynthesizer->m_pTG[nTG]->setLFODelay(scale(99, pMessage[2]));

											break;
										case  DEXED_LFO_PITCH_MOD_DEP:
											m_pSynthesizer->m_pTG[nTG]->setLFOPitchModulationDepth(scale(99, pMessage[2]));

											break;
										case  DEXED_LFO_AMP_MOD_DEP:
											m_pSynthesizer->m_pTG[nTG]->setLFOAmpModulationDepth(scale(99, pMessage[2]));

											break;
										case  DEXED_LFO_SYNC:
											m_pSynthesizer->m_pTG[nTG]->setLFOSync(scale(1, pMessage[2]));

											break;
										case  DEXED_LFO_WAVE:
											m_pSynthesizer->m_pTG[nTG]->setLFOWaveform(scale(4, pMessage[2]));

											break;
										case  DEXED_LFO_PITCH_MOD_SENS:
											m_pSynthesizer->m_pTG[nTG]->setLFOPitchModulationSensitivity(scale(7, pMessage[2]));

											break;
										case  DEXED_TRANSPOSE:
											m_pSynthesizer->m_pTG[nTG]->setTranspose(scale(48, pMessage[2]));

											break;
									}
									// TODO: send partial update, but this will do for now
									SendSystemExclusiveVoice(0, 0, nTG);
								}
							}
							break;

						}
						break;
		
					case MIDI_PROGRAM_CHANGE:
						// do program change only if enabled in config and not in "Performance Select Channel" mode
						if( m_pConfig->GetMIDIRXProgramChange() && ( m_pSynthesizer->GetPerformanceSelectChannel() == Disabled) ) {
							//printf("Program Change to %d (%d)\n", ucChannel, m_pSynthesizer->GetPerformanceSelectChannel());
							m_pSynthesizer->ProgramChange (pMessage[1], nTG);
						}
						break;
		
					case MIDI_PITCH_BEND: {
						if (nLength < 3)
						{
							break;
						}
		
						s16 nValue = pMessage[1];
						nValue |= (s16) pMessage[2] << 7;
						nValue -= 0x2000;
		
						m_pSynthesizer->setPitchbend (nValue, nTG);
						} break;
		
					default:
						break;
					}
				}
			}
		}
	}
	m_MIDISpinLock.Release ();
}

void CMIDIDevice::AddDevice (const char *pDeviceName)
{
	assert (pDeviceName);

	assert (m_DeviceName.empty ());
	m_DeviceName = pDeviceName;
	assert (!m_DeviceName.empty ());

	s_DeviceMap.insert (std::pair<std::string, CMIDIDevice *> (pDeviceName, this));
}

void CMIDIDevice::HandleSystemExclusive(const uint8_t* pMessage, const size_t nLength, const unsigned nCable, const uint8_t nTG)
{
  int16_t sysex_return;

  sysex_return = m_pSynthesizer->checkSystemExclusive(pMessage, nLength, nTG);
  LOGDBG("SYSEX handler return value: %d", sysex_return);

  switch (sysex_return)
  {
    case -1:
      LOGERR("SysEx end status byte not detected.");
      break;
    case -2:
      LOGERR("SysEx vendor not Yamaha.");
      break;
    case -3:
      LOGERR("Unknown SysEx parameter change.");
      break;
    case -4:
      LOGERR("Unknown SysEx voice or function.");
      break;
    case -5:
      LOGERR("Not a SysEx voice bulk upload.");
      break;
    case -6:
      LOGERR("Wrong length for SysEx voice bulk upload (not 155).");
      break;
    case -7:
      LOGERR("Checksum error for one voice.");
      break;
    case -8:
      LOGERR("Not a SysEx bank bulk upload.");
      break;
    case -9:
      LOGERR("Wrong length for SysEx bank bulk upload (not 4096).");
    case -10:
      LOGERR("Checksum error for bank.");
      break;
    case -11:
      LOGERR("Unknown SysEx message.");
      break;
    case 64:
      LOGDBG("SysEx Function parameter change: %d Value %d",pMessage[4],pMessage[5]);
      m_pSynthesizer->setMonoMode(pMessage[5],nTG);
      break;
    case 65:
      LOGDBG("SysEx Function parameter change: %d Value %d",pMessage[4],pMessage[5]);
      m_pSynthesizer->setPitchbendRange(pMessage[5],nTG);
      break;
    case 66:
      LOGDBG("SysEx Function parameter change: %d Value %d",pMessage[4],pMessage[5]);
      m_pSynthesizer->setPitchbendStep(pMessage[5],nTG);
      break;
    case 67:
      LOGDBG("SysEx Function parameter change: %d Value %d",pMessage[4],pMessage[5]);
      m_pSynthesizer->setPortamentoMode(pMessage[5],nTG);
      break;
    case 68:
      LOGDBG("SysEx Function parameter change: %d Value %d",pMessage[4],pMessage[5]);
      m_pSynthesizer->setPortamentoGlissando(pMessage[5],nTG);
      break;
    case 69:
      LOGDBG("SysEx Function parameter change: %d Value %d",pMessage[4],pMessage[5]);
      m_pSynthesizer->setPortamentoTime(pMessage[5],nTG);
      break;
    case 70:
      LOGDBG("SysEx Function parameter change: %d Value %d",pMessage[4],pMessage[5]);
      m_pSynthesizer->setModWheelRange(pMessage[5],nTG);
      break;
    case 71:
      LOGDBG("SysEx Function parameter change: %d Value %d",pMessage[4],pMessage[5]);
      m_pSynthesizer->setModWheelTarget(pMessage[5],nTG);
      break;
    case 72:
      LOGDBG("SysEx Function parameter change: %d Value %d",pMessage[4],pMessage[5]);
      m_pSynthesizer->setFootControllerRange(pMessage[5],nTG);
      break;
    case 73:
      LOGDBG("SysEx Function parameter change: %d Value %d",pMessage[4],pMessage[5]);
      m_pSynthesizer->setFootControllerTarget(pMessage[5],nTG);
      break;
    case 74:
      LOGDBG("SysEx Function parameter change: %d Value %d",pMessage[4],pMessage[5]);
      m_pSynthesizer->setBreathControllerRange(pMessage[5],nTG);
      break;
    case 75:
      LOGDBG("SysEx Function parameter change: %d Value %d",pMessage[4],pMessage[5]);
      m_pSynthesizer->setBreathControllerTarget(pMessage[5],nTG);
      break;
    case 76:
      LOGDBG("SysEx Function parameter change: %d Value %d",pMessage[4],pMessage[5]);
      m_pSynthesizer->setAftertouchRange(pMessage[5],nTG);
      break;
    case 77:
      LOGDBG("SysEx Function parameter change: %d Value %d",pMessage[4],pMessage[5]);
      m_pSynthesizer->setAftertouchTarget(pMessage[5],nTG);
      break;
    case 100:
      // load sysex-data into voice memory
      LOGDBG("One Voice bulk upload");
      m_pSynthesizer->loadVoiceParameters(pMessage,nTG);
      break;
    case 200:
      LOGDBG("Bank bulk upload.");
      //TODO: add code for storing a bank bulk upload
      LOGNOTE("Currently code  for storing a bulk bank upload is missing!");
      break;
    default:
      if(sysex_return >= 300 && sysex_return < 500)
      {
        LOGDBG("SysEx voice parameter change: Parameter %d value: %d",pMessage[4] + ((pMessage[3] & 0x03) * 128), pMessage[5]);
        m_pSynthesizer->setVoiceDataElement(pMessage[4] + ((pMessage[3] & 0x03) * 128), pMessage[5],nTG);
        switch(pMessage[4] + ((pMessage[3] & 0x03) * 128))
        {
          case 134:
            m_pSynthesizer->notesOff(0,nTG);
            break;
        }
      }
      else if(sysex_return >= 500 && sysex_return < 600)
      {
        LOGDBG("SysEx send voice %u request",sysex_return-500);
        SendSystemExclusiveVoice(sysex_return-500, nCable, nTG);
      }
      break;
  }
}

void CMIDIDevice::SendSystemExclusiveVoice(uint8_t nVoice, const unsigned nCable, uint8_t nTG)
{
  uint8_t voicedump[163];

  // Get voice sysex dump from TG
  m_pSynthesizer->getSysExVoiceDump(voicedump, nTG);

  TDeviceMap::const_iterator Iterator;

  // send voice dump to all MIDI interfaces
  for(Iterator = s_DeviceMap.begin(); Iterator != s_DeviceMap.end(); ++Iterator)
  {
    Iterator->second->Send (voicedump, sizeof(voicedump)*sizeof(uint8_t));
    // LOGDBG("Send SYSEX voice dump %u to \"%s\"",nVoice,Iterator->first.c_str());
  }
} 
