using System;
using System.Collections.Generic;
using System.Linq;
using System.Text;
using System.Threading.Tasks;
using Sys = Cosmos.System;

namespace Jupiter
{
    public class Commands
    {
        public static void PowerOff()
        {
            Sys.Power.Shutdown();
        }
        public static void Reboot() 
        {
            Sys.Power.Reboot();
        }
        public static void Clear() 
        {
            Console.Clear();
        }
        public static void NotFound()
        {
            Console.WriteLine("Command not found!");
        }
    }
}
