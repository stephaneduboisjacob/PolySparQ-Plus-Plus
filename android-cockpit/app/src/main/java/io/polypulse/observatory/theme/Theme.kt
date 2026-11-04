package io.polypulse.observatory.theme

import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.darkColorScheme
import androidx.compose.runtime.Composable

private val ObservatoryColors = darkColorScheme(
    primary = Mint,
    onPrimary = Night,
    secondary = Aqua,
    tertiary = Gold,
    background = Night,
    onBackground = Ink,
    surface = Surface,
    onSurface = Ink,
    surfaceVariant = SurfaceRaised,
    onSurfaceVariant = Muted,
    outline = Stroke,
    error = Coral,
)

@Composable
fun PolySparQObservatoryTheme(
  darkTheme: Boolean = true,
  dynamicColor: Boolean = false,
  content: @Composable () -> Unit,
) {
  MaterialTheme(colorScheme = ObservatoryColors, typography = Typography, content = content)
}
