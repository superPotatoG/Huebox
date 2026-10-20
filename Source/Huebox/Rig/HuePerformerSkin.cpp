#include "Rig/HuePerformerSkin.h"

#include "Animation/AnimSequence.h"
#include "Engine/SkeletalMesh.h"
#include "Materials/MaterialInterface.h"

namespace HueSkinPrivate
{
	const TCHAR* const MannequinPath = TEXT("/Game/Characters/Mannequins/Meshes/SKM_Manny_Simple.SKM_Manny_Simple");
	const TCHAR* const JogPath = TEXT("/Game/Characters/Mannequins/Anims/Unarmed/Jog/MF_Unarmed_Jog_Fwd.MF_Unarmed_Jog_Fwd");
	const TCHAR* const IdlePath = TEXT("/Game/Characters/Mannequins/Anims/Unarmed/MM_Idle.MM_Idle");
}

UHuePerformerSkin::UHuePerformerSkin()
{
	DisplayName = FText::FromString(TEXT("Skin"));
}

UHueSkin_Silhouette::UHueSkin_Silhouette()
{
	using namespace HueSkinPrivate;

	DisplayName = FText::FromString(TEXT("Silhouette Dancer"));
	PerformerName = TEXT("Dancer");

	bShowCarriageBody = false;
	bShowAnchorBlock = false;
	StructureColor = FLinearColor(0.02f, 0.02f, 0.02f);
	bLettersInSignalColor = true;

	PerformerMesh = TSoftObjectPtr<USkeletalMesh>(FSoftObjectPath(MannequinPath));
	PerformerMoveAnim = TSoftObjectPtr<UAnimSequence>(FSoftObjectPath(JogPath));
	PerformerIdleAnim = TSoftObjectPtr<UAnimSequence>(FSoftObjectPath(IdlePath));
	PerformerScale = 0.7f;
	bFaceMotionDirection = true;
	bFlatPerformerColor = true;
	PerformerColor = FLinearColor(0.01f, 0.01f, 0.01f);

	bShowPartner = true;
	PartnerMesh = PerformerMesh;
	PartnerAnim = PerformerIdleAnim;

	bShowBackdrop = true;
	bZoneAsSpotlight = true;
}

UHueSkin_Carriage::UHueSkin_Carriage()
{
	using namespace HueSkinPrivate;

	DisplayName = FText::FromString(TEXT("Prototype Carriage"));
	PerformerName = TEXT("Carriage");

	PerformerMesh = TSoftObjectPtr<USkeletalMesh>(FSoftObjectPath(MannequinPath));
	PerformerScale = 0.45f;
}
