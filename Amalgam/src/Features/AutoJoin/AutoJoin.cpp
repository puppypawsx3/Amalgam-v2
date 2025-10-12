#include "AutoJoin.h"

#include <algorithm>
#include <vector>

// Doesnt work with custom huds!1!!
void CAutoJoin::Run(CTFPlayer* pLocal)
{
	static Timer tJoinTimer{};
	static Timer tRandomTimer{};
	static int iRandomClass = 0;

	if (!pLocal)
	{
		iRandomClass = 0;
		return;
	}

	int iDesiredClass = 0;
	const bool bRandomEnabled = Vars::Misc::Automation::RandomClass.Value;

	if (bRandomEnabled)
	{
		const int iExcludeMask = Vars::Misc::Automation::RandomClassExclude.Value;
		std::vector<int> vCandidates{};
		vCandidates.reserve(m_aClassNames.size());
		for (int i = 0; i < static_cast<int>(m_aClassNames.size()); i++)
		{
			if (!(iExcludeMask & (1 << i)))
			{
				vCandidates.push_back(i + 1);
			}
		}

		if (!vCandidates.empty())
		{
			if (std::find(vCandidates.begin(), vCandidates.end(), iRandomClass) == vCandidates.end())
			{
				iRandomClass = 0;
			}

			const float flIntervalSeconds = std::max(1.f, Vars::Misc::Automation::RandomClassInterval.Value * 60.f);
			bool bPickNew = iRandomClass == 0;
			bool bPickedByTimer = false;
			if (!bPickNew && tRandomTimer.Run(flIntervalSeconds))
			{
				bPickNew = true;
				bPickedByTimer = true;
			}

			if (bPickNew)
			{
				const int iSelection = SDK::RandomInt(0, static_cast<int>(vCandidates.size()) - 1);
				iRandomClass = vCandidates[iSelection];
				if (!bPickedByTimer)
				{
					tRandomTimer.Update();
				}
			}

			iDesiredClass = iRandomClass;
		}
		else
		{
			iRandomClass = 0;
		}
	}
	else
	{
		iRandomClass = 0;
	}

	if (!iDesiredClass)
	{
		iDesiredClass = Vars::Misc::Automation::ForceClass.Value;
	}

	if (!iDesiredClass)
	{
		return;
	}

	if (iDesiredClass < 1 || iDesiredClass > static_cast<int>(m_aClassNames.size()))
	{
		return;
	}

	if (tJoinTimer.Run(1.f))
	{
		if (pLocal->IsInValidTeam())
		{
			I::EngineClient->ClientCmd_Unrestricted(std::format("joinclass {}", m_aClassNames[iDesiredClass - 1]).c_str());
			I::EngineClient->ClientCmd_Unrestricted("menuclosed");
		}
		else
		{
			I::EngineClient->ClientCmd_Unrestricted("team_ui_setup");
			I::EngineClient->ClientCmd_Unrestricted("menuopen");
			I::EngineClient->ClientCmd_Unrestricted("autoteam");
			I::EngineClient->ClientCmd_Unrestricted("menuclosed");
		}
	}
}