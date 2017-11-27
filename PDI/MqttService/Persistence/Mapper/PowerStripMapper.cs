﻿using System.Collections.Generic;
using System.Linq;
using MqttService.Models;
using MqttService.Persistence.Entity;

namespace MqttService.Persistence.Mapper
{
    public class PowerStripMapper
    {
        public PowerStripEntity Map(PowerStrip ps)
        {
            return new PowerStripEntity
            {
                DeviceId = ps.DeviceId,
                Powered  = ps.Powered
            };
        }

        public IEnumerable<PowerStrip> Map(IEnumerable<PowerStripEntity> powerstrips)
        {
            return powerstrips.Select(
                ps => new PowerStrip(ps.DeviceId, ps.Powered)
            ).ToArray();
        }
    }
}
