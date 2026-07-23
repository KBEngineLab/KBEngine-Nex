// #pragma once

// #include "KBECommon.h"


// #include "KBEvent.h"
// #include "Components/ActorComponent.h"
// #include "UKBEMain.generated.h"


// class KBEngineApp;

// UENUM(BlueprintType)
// enum class UE_CLIENT_TYPE : uint8
// {
// 	CLIENT_TYPE_UNKNOWN		UMETA(DisplayName = "unknown"),
// 	CLIENT_TYPE_MOBILE		UMETA(DisplayName = "Mobile"),
// 	CLIENT_TYPE_WIN			UMETA(DisplayName = "Windows"),
// 	CLIENT_TYPE_LINUX		UMETA(DisplayName = "Linux"),
// 	CLIENT_TYPE_MAC			UMETA(DisplayName = "Mac"),
// 	CLIENT_TYPE_BROWSER		UMETA(DisplayName = "Browser"),
// 	CLIENT_TYPE_BOTS		UMETA(DisplayName = "Bots"),
// 	CLIENT_TYPE_MINI		UMETA(DisplayName = "Mini"),
// };

// UENUM(BlueprintType)
// enum class UE_NETWORK_ENCRYPT_TYPE : uint8
// {
// 	ENCRYPT_TYPE_NONE			UMETA(DisplayName = "None"),
// 	ENCRYPT_TYPE_BLOWFISH		UMETA(DisplayName = "Blowfish"),
// };

// UCLASS(ClassGroup = "KBEngine", blueprintable, editinlinenew, hidecategories = (Object, LOD, Lighting, TextureStreaming), meta = (DisplayName = "KBEngine Main", BlueprintSpawnableComponent))
// class  UKBEMain : public UActorComponent
// {
// 	GENERATED_BODY()

// public:	
// 	UKBEMain();


// 	virtual void InitializeComponent() override;

// 	virtual void BeginPlay() override;

	
// 	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

	
// 	virtual void UninitializeComponent() override;

// 	virtual void TickComponent( float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction ) override;


// 	// void installEvents();
// 	// void deregisterEvents();

// 	// void onVersionNotMatch(std::shared_ptr<UKBEventData> pEventData);
// 	// void onScriptVersionNotMatch(std::shared_ptr<UKBEventData> pEventData);

// 	UFUNCTION(BlueprintCallable, Category = "KBEngine")
// 	static FString getClientVersion();

// 	UFUNCTION(BlueprintCallable, Category = "KBEngine")
// 	static FString getClientScriptVersion();

// 	UFUNCTION(BlueprintCallable, Category = "KBEngine")
// 	static FString getServerVersion();

// 	UFUNCTION(BlueprintCallable, Category = "KBEngine")
// 	static FString getServerScriptVersion();

	
// 	UFUNCTION(BlueprintCallable, Category = "KBEngine")
// 	static FString getComponentName();

	
// 	UFUNCTION(BlueprintCallable, Category = "KBEngine")
// 	bool destroyKBEngine();

// 	UFUNCTION(BlueprintCallable, Category = "KBEngine")
// 	bool login(FString username, FString password, TArray<uint8> datas);


// 	UFUNCTION(BlueprintCallable, Category = "KBEngine")
// 	bool createAccount(FString username, FString password, const TArray<uint8>& datas);

// 	UFUNCTION(BlueprintCallable, Category = "KBEngine")
// 	bool resetPassword(FString username);

// 	UFUNCTION(BlueprintCallable, Category = "KBEngine")
// 	bool bindAccountEmail(FString email);

// 	UFUNCTION(BlueprintCallable, Category = "KBEngine")
// 	bool newPassword(FString oldPassword, FString newPassword);

// 	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = KBEngine)
// 	FString ip;

// 	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = KBEngine)
// 	int port;

// 	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = KBEngine)
// 	UE_CLIENT_TYPE clientType;

// 	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = KBEngine)
// 	UE_NETWORK_ENCRYPT_TYPE networkEncryptType;

// 	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = KBEngine)
// 	int syncPlayerMS;

// 	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = KBEngine)
// 	bool useAliasEntityID;

// 	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = KBEngine)
// 	bool isOnInitCallPropertysSetMethods;

// 	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = KBEngine)
// 	bool forceDisableUDP;

// 	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = KBEngine)
// 	int serverHeartbeatTick;

// 	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = KBEngine)
// 	int TCP_SEND_BUFFER_MAX;

// 	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = KBEngine)
// 	int TCP_RECV_BUFFER_MAX;

// 	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = KBEngine)
// 	int UDP_SEND_BUFFER_MAX;

// 	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = KBEngine)
// 	int UDP_RECV_BUFFER_MAX;

// 	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = KBEngine)
// 	bool disableMainLoop;
// };

