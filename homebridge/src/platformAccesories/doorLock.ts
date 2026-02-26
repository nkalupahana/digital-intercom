import { HAP, PlatformAccessory, Service } from "homebridge";
import { DigitalIntercomPlatform } from "../platform.js";
import { Command } from "../constants.js";

export const DOOR_LOCK_ACCESSORY_NAME = "Door Lock";

export class DoorLockPlatformAccessory {
  private hap: HAP;
  private accessory: PlatformAccessory;
  private service: Service;

  constructor(
    private readonly platform: DigitalIntercomPlatform,
    private readonly paccessory: PlatformAccessory,
  ) {
    this.hap = this.platform.api.hap;
    this.accessory = paccessory;

    this.service =
      this.accessory.getService(this.hap.Service.LockMechanism) ||
      this.accessory.addService(this.hap.Service.LockMechanism);

    this.service.setCharacteristic(
      this.platform.Characteristic.Name,
      this.accessory.displayName,
    );

    this.service
      .getCharacteristic(this.platform.Characteristic.LockCurrentState)
      .onGet(() => {
        // stateless -- always shows as locked
        return this.platform.Characteristic.LockCurrentState.SECURED;
      });

    this.service
      .getCharacteristic(this.platform.Characteristic.LockTargetState)
      .onGet(() => {
        // stateless -- always shows as locked
        return this.platform.Characteristic.LockTargetState.SECURED;
      })
      .onSet((value) => {
        if (value === this.platform.Characteristic.LockTargetState.UNSECURED) {
          this.platform.server.sendCommand(Command.OPEN_DOOR);
        }
      });
  }
}
