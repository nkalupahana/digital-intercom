import type {
  API,
  Characteristic,
  DynamicPlatformPlugin,
  Logging,
  PlatformAccessory,
  Service,
} from "homebridge";

import {
  IntercomPlatformAccessory,
  INTERCOM_ACCESSORY_NAME,
} from "./platformAccesories/intercom.js";
import {
  DOOR_LOCK_ACCESSORY_NAME,
  DoorLockPlatformAccessory,
} from "./platformAccesories/doorLock.js";
import { PLATFORM_NAME, PLUGIN_NAME } from "./settings.js";
import { Server } from "./server.js";
import { DigitalIntercomPlatformConfig } from "./constants.js";

export class DigitalIntercomPlatform implements DynamicPlatformPlugin {
  public readonly Service: typeof Service;
  public readonly Characteristic: typeof Characteristic;

  // this is used to track restored cached accessories
  public readonly accessories: Map<string, PlatformAccessory> = new Map();
  public readonly discoveredCacheUUIDs: string[] = [];

  // This is only required when using Custom Services and Characteristics not support by HomeKit
  // eslint-disable-next-line @typescript-eslint/no-explicit-any
  public readonly CustomServices: any;
  // eslint-disable-next-line @typescript-eslint/no-explicit-any
  public readonly CustomCharacteristics: any;
  public readonly config: DigitalIntercomPlatformConfig;
  public readonly server: Server;
  private intercomPlatformAccessory: IntercomPlatformAccessory | null = null;

  constructor(
    public readonly log: Logging,
    public readonly _config: DigitalIntercomPlatformConfig,
    public readonly api: API,
  ) {
    this.Service = api.hap.Service;
    this.Characteristic = api.hap.Characteristic;
    this.config = _config;

    // When this event is fired it means Homebridge has restored all cached accessories from disk.
    // Dynamic Platform plugins should only register new accessories after this event was fired,
    // in order to ensure they weren't added to homebridge already. This event can also be used
    // to start discovery of new accessories.
    this.api.on("didFinishLaunching", () => {
      log.debug("Executed didFinishLaunching callback");
      // run the method to discover / register your devices as accessories
      this.discoverDevices();
    });

    this.server = new Server(this);
  }

  /**
   * This function is invoked when homebridge restores cached accessories from disk at startup.
   * It should be used to set up event handlers for characteristics and update respective values.
   */
  configureAccessory(accessory: PlatformAccessory) {
    this.log.info("Loading accessory from cache:", accessory.displayName);

    // add the restored accessory to the accessories cache, so we can track if it has already been registered
    this.accessories.set(accessory.UUID, accessory);
  }

  discoverDevices() {
    const intercomUuid = this.api.hap.uuid.generate(INTERCOM_ACCESSORY_NAME);
    let intercomAccesory = this.accessories.get(intercomUuid);
    const accesoriesToCreate = [];
    let intercomPlatformAccessory: IntercomPlatformAccessory;
    if (intercomAccesory) {
      this.log.info("Restoring intercom accessory from cache");
      intercomPlatformAccessory = new IntercomPlatformAccessory(
        this,
        intercomAccesory,
      );
    } else {
      this.log.info("Adding new intercom accessory");
      intercomAccesory = new this.api.platformAccessory(
        INTERCOM_ACCESSORY_NAME,
        intercomUuid,
      );
      intercomPlatformAccessory = new IntercomPlatformAccessory(
        this,
        intercomAccesory,
      );
      accesoriesToCreate.push(intercomAccesory);
    }

    const doorLockUuid = this.api.hap.uuid.generate(DOOR_LOCK_ACCESSORY_NAME);
    const existingDoorLock = this.accessories.get(doorLockUuid);
    if (existingDoorLock) {
      this.log.info("Restoring door lock accessory from cache");
      new DoorLockPlatformAccessory(this, existingDoorLock);
    } else {
      this.log.info("Adding new door lock accessory");
      const doorLockAccessory = new this.api.platformAccessory(
        DOOR_LOCK_ACCESSORY_NAME,
        doorLockUuid,
      );
      new DoorLockPlatformAccessory(this, doorLockAccessory);
      accesoriesToCreate.push(doorLockAccessory);
    }

    if (accesoriesToCreate.length > 0) {
      this.api.registerPlatformAccessories(
        PLUGIN_NAME,
        PLATFORM_NAME,
        accesoriesToCreate,
      );
    }

    this.intercomPlatformAccessory = intercomPlatformAccessory;
  }

  public triggerDoorbell() {
    if (!this.intercomPlatformAccessory) {
      this.log.error(
        "Cannot trigger doorbell because intercom accessory not set up",
      );
      return;
    }

    this.intercomPlatformAccessory.triggerDoorbell();
  }
}
