using Microsoft.Win32.SafeHandles;
using System;
using System.Collections.Generic;
using System.Text;
using Sys = Cosmos.System;

namespace Jupiter
{
    public class Kernel : Sys.Kernel
    {

        protected override void BeforeRun()
        {
            Console.Clear();
            // Console.WriteLine("Jupiter booted successfully.");
        }

        protected override void Run()
        {
            Console.Write("Jupiter# ");
            var input = Console.ReadLine().ToLower().Trim();
            switch(input) 
            {
                case "": break;
                case "poweroff":
                case "shutdown":
                    Commands.PowerOff(); break;
                case "reboot":
                case "restart":
                    Commands.Reboot(); break;
                case "clear":
                case "pak":
                    Commands.Clear(); break;
                default:
                    Commands.NotFound(); break;
            }
        }
    }
}
