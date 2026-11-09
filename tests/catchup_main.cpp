int check_catchup(int argc, char **argv);

int main(int argc, char **argv) {
    return check_catchup(argc - 1, argv + 1);
}
