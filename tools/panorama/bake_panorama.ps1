# Bakes the main menu's 360 degree panorama (ui/panorama.jpg) from the game:
# one run per view direction (four around, up and down) at a 90 degree field of
# view, each window captured, then stitched into one equirectangular image
# (longitude across, latitude down; the menu's shader reads it the same way).
#
#   powershell -ExecutionPolicy Bypass -File tools/panorama/bake_panorama.ps1 `
#       -Exe out/VoxelRethinked/voxel_rethinked.exe -Out textures/default/ui/panorama.jpg `
#       -X 2366 -Y 82 -Z 2168 -Width 4096
#
# The menu picks one of ui/panorama.jpg and ui/panorama_2.jpg to _4.jpg at
# random each launch. Each view waits for the world to load, then moves the
# camera and waits for the land around it. Time stands still, so clouds match
# across the six views. Sun rays are off: they brighten only the view that faces
# the sun, which would show as a seam. The window is a large square (-Window), as each view
# only uses a square of the window height.
#
# Run it from a packaged copy (build.cmd release package, out/VoxelRethinked), never
# from the folder someone plays from: settings the bake changes can stick there.
# Run it again whenever the world should look different (real world generation,
# other times of day). The game is stopped rather than closed, so its settings
# file stays as it was. A window pops up six times; leave the keyboard alone.
param([string]$Exe, [string]$Out, [double]$X = 40, [double]$Y = 110, [double]$Z = 90, [int]$WaitMs = 45000,
      [int]$Width = 4096, [string]$TimeOfDay = "10", [string]$Weather = "Fair", [int]$Window = 1440)

$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Drawing
Add-Type -ReferencedAssemblies System.Drawing -TypeDefinition @"
using System;
using System.Drawing;
using System.Drawing.Imaging;
using System.Runtime.InteropServices;

public static class Panorama {
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
    [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr hWnd, out RECT rect);
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr hWnd, out RECT rect);
    [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr hWnd, ref POINT point);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr hWnd, IntPtr hdc, uint flags);

    // The client area of a window (no title bar or borders).
    public static Bitmap Capture(IntPtr window) {
        RECT frame, client;
        GetWindowRect(window, out frame);
        GetClientRect(window, out client);
        POINT origin = new POINT();
        ClientToScreen(window, ref origin);
        using (Bitmap full = new Bitmap(frame.Right - frame.Left, frame.Bottom - frame.Top)) {
            using (Graphics graphics = Graphics.FromImage(full)) {
                IntPtr hdc = graphics.GetHdc();
                PrintWindow(window, hdc, 2);
                graphics.ReleaseHdc(hdc);
            }
            Rectangle area = new Rectangle(origin.X - frame.Left, origin.Y - frame.Top, client.Right, client.Bottom);
            return full.Clone(area, PixelFormat.Format32bppArgb);
        }
    }

    class View {
        public int[] Pixels; public int Width, Height;
        public double[] Forward, Right, Up;
    }

    static double Dot(double[] a, double[] b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
    static double[] Cross(double[] a, double[] b) {
        return new[] { a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0] };
    }

    // Camera axes as the engine computes them (render::Camera), yaw and pitch in degrees.
    static View MakeView(Bitmap image, double yawDegrees, double pitchDegrees) {
        double yaw = yawDegrees * Math.PI / 180.0, pitch = pitchDegrees * Math.PI / 180.0;
        double[] forward = { Math.Sin(yaw) * Math.Cos(pitch), Math.Sin(pitch), -Math.Cos(yaw) * Math.Cos(pitch) };
        double[] right = { Math.Cos(yaw), 0.0, Math.Sin(yaw) };
        BitmapData data = image.LockBits(new Rectangle(0, 0, image.Width, image.Height), ImageLockMode.ReadOnly,
                                         PixelFormat.Format32bppArgb);
        int[] pixels = new int[image.Width * image.Height];
        for (int row = 0; row < image.Height; ++row) {
            Marshal.Copy(data.Scan0 + row * data.Stride, pixels, row * image.Width, image.Width);
        }
        image.UnlockBits(data);
        return new View { Pixels = pixels, Width = image.Width, Height = image.Height, Forward = forward,
                          Right = right, Up = Cross(right, forward) };
    }

    // One color channel of a view at a fractional pixel, bilinear.
    static double Sample(View view, double x, double y, int shift) {
        x = Math.Max(0.0, Math.Min(view.Width - 1.001, x));
        y = Math.Max(0.0, Math.Min(view.Height - 1.001, y));
        int ix = (int)x, iy = (int)y;
        double fx = x - ix, fy = y - iy;
        Func<int, int, double> at = (px, py) => (view.Pixels[py * view.Width + px] >> shift) & 255;
        return (at(ix, iy) * (1 - fx) + at(ix + 1, iy) * fx) * (1 - fy) +
               (at(ix, iy + 1) * (1 - fx) + at(ix + 1, iy + 1) * fx) * fy;
    }

    // images: views in the order of yaws and pitches. Every view has a 90
    // degree vertical field of view: a square of the window height around the
    // center covers its face of the cube; wider windows just waste the sides.
    public static Bitmap Stitch(Bitmap[] images, double[] yaws, double[] pitches, int width) {
        View[] views = new View[images.Length];
        for (int i = 0; i < images.Length; ++i) {
            views[i] = MakeView(images[i], yaws[i], pitches[i]);
        }
        int height = width / 2;
        int[] output = new int[width * height];
        for (int row = 0; row < height; ++row) {
            double latitude = (0.5 - (row + 0.5) / height) * Math.PI;
            for (int column = 0; column < width; ++column) {
                double longitude = (column + 0.5) / width * 2.0 * Math.PI;
                double[] ray = { Math.Sin(longitude) * Math.Cos(latitude), Math.Sin(latitude),
                                 -Math.Cos(longitude) * Math.Cos(latitude) };
                View best = views[0];
                double bestDot = -2.0;
                foreach (View view in views) {
                    double dot = Dot(ray, view.Forward);
                    if (dot > bestDot) { bestDot = dot; best = view; }
                }
                double half = best.Height / 2.0;
                double x = best.Width / 2.0 + Dot(ray, best.Right) / bestDot * half - 0.5;
                double y = half - Dot(ray, best.Up) / bestDot * half - 0.5;
                int red = (int)Sample(best, x, y, 16), green = (int)Sample(best, x, y, 8), blue = (int)Sample(best, x, y, 0);
                output[row * width + column] = (255 << 24) | (red << 16) | (green << 8) | blue;
            }
        }
        Bitmap result = new Bitmap(width, height, PixelFormat.Format32bppArgb);
        BitmapData target = result.LockBits(new Rectangle(0, 0, width, height), ImageLockMode.WriteOnly,
                                            PixelFormat.Format32bppArgb);
        for (int row = 0; row < height; ++row) {
            Marshal.Copy(output, row * width, target.Scan0 + row * target.Stride, width);
        }
        result.UnlockBits(target);
        return result;
    }
}
"@

$yaws = 0.0, 90.0, 180.0, 270.0, 0.0, 0.0
$pitches = 0.0, 0.0, 0.0, 0.0, 89.99, -89.99
$executable = (Resolve-Path $Exe).Path
$directory = Split-Path $executable
$images = New-Object 'System.Drawing.Bitmap[]' 6
for ($i = 0; $i -lt 6; ++$i) {
    # Frozen: real mouse and keys cannot move the camera during the wait.
    $commands = "set Video.WindowMode Windowed", "set Video.WindowSize 0",
                "set Debug.Overlay.Visible false", "set Debug.FreeCamera.Frozen true",
                "set Gameplay.FieldOfView 90", "set Interface.ShowHud false", "set Video.ShowFps false",
                "set Gameplay.ViewBobbing false", "set Graphics.Sky.GodRays false", "set Graphics.Sky.DayMinutes 0", "set Graphics.Sky.Weather $Weather",
                "preset Graphics.LOD High", "play", "wait 3000", "set Graphics.Sky.TimeOfDay $TimeOfDay",
                "camera $X $Y $Z $($yaws[$i]) $($pitches[$i])"
    $arguments = @("--window", "$($Window)x$($Window)")
    foreach ($command in $commands) { $arguments += @("--exec", "`"$command`"") }
    # A view is tried again if the game has no window to capture (it is still
    # starting, or it stopped), rather than failing the whole bake.
    for ($attempt = 1; $attempt -le 3 -and $null -eq $images[$i]; ++$attempt) {
        $process = Start-Process -FilePath $executable -ArgumentList $arguments -WorkingDirectory $directory -PassThru
        Start-Sleep -Milliseconds $WaitMs
        for ($wait = 0; $wait -lt 40 -and $null -eq $images[$i] -and -not $process.HasExited; ++$wait) {
            $process.Refresh()
            if ($process.MainWindowHandle -ne [IntPtr]::Zero) {
                try { $images[$i] = [Panorama]::Capture($process.MainWindowHandle) } catch { $images[$i] = $null }
            }
            if ($null -eq $images[$i]) { Start-Sleep -Milliseconds 500 }
        }
        if (-not $process.HasExited) { Stop-Process -Id $process.Id -Force }
        $process.WaitForExit()
        if ($null -eq $images[$i]) { Write-Output "view $($i + 1): no window to capture, attempt $attempt" }
    }
    if ($null -eq $images[$i]) { throw "view $($i + 1) could not be captured" }
    Write-Output "view $($i + 1) of 6 captured ($($images[$i].Width) x $($images[$i].Height))"
}
$result = [Panorama]::Stitch($images, [double[]]$yaws, [double[]]$pitches, $Width)
$encoder = [System.Drawing.Imaging.ImageCodecInfo]::GetImageEncoders() | Where-Object { $_.MimeType -eq "image/jpeg" }
$parameters = New-Object System.Drawing.Imaging.EncoderParameters 1
$parameters.Param[0] = New-Object System.Drawing.Imaging.EncoderParameter ([System.Drawing.Imaging.Encoder]::Quality, [long]90)
$path = [System.IO.Path]::GetFullPath($Out)
$result.Save($path, $encoder, $parameters)
Write-Output "wrote $path"
