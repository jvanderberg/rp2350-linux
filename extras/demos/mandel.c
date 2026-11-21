#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
    const int width = 78;
    const int height = 28;
    const char shades[] = " .:-=+*#%@";
    const int shade_count = sizeof(shades) - 1;
    int max_iter = 60;

    if (argc > 1) {
        max_iter = atoi(argv[1]);
        if (max_iter < 1 || max_iter > 1000) {
            fprintf(stderr, "Usage: %s [iterations: 1-1000]\n",
                    argv[0]);
            return 1;
        }
    }

    for (int row = 0; row < height; row++) {
        double cy = -1.1 + 2.2 * row / (height - 1);

        for (int col = 0; col < width; col++) {
            double cx = -2.2 + 3.2 * col / (width - 1);
            double x = 0.0;
            double y = 0.0;
            int iter = 0;

            /* Repeatedly calculate z = z*z + c. */
            while (x*x + y*y <= 4.0 && iter < max_iter) {
                double next_x = x*x - y*y + cx;
                y = 2.0*x*y + cy;
                x = next_x;
                iter++;
            }

            if (iter == max_iter) {
                putchar('@');
            } else {
                int shade = iter * (shade_count - 1) / max_iter;
                putchar(shades[shade]);
            }
        }

        putchar('\n');
        fflush(stdout); /* Show each row as it finishes. */
    }

    return 0;
}
