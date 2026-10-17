/* The program's entry: the game itself is dqix_main (main.c), so the same
 * code can also run as a library (hosting.c). */
int dqix_main(int argc, char** argv);
int main(int argc, char** argv) { return dqix_main(argc, argv); }
