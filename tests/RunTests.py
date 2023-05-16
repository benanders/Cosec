
import subprocess, sys, os, re

nasm_bin = "nasm"
nasm_args = ["-f", "macho64"]
ld_bin = "ld"
ld_args = ["-L/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk/usr/lib", "-lSystem"]

def run_test(cosec_bin, path):
    f = open(path, "r")
    contents = f.read()

    # Find expected return code
    re_search = re.search(r'\/\/ expect\: (\d+)', contents)
    if re_search is None:
        print("  Test '" + path + "': no '// expect: ' in file")
        return
    groups = re_search.groups()
    if len(groups) == 0:
        print("  Test '" + path + "': no matching regex '// expect: (\\d+)' in file")
        return
    expected_output = int(groups[0])

    # Compile
    result = subprocess.run([cosec_bin, path, "-o", "out.s"], capture_output=True, text=True)
    if result.returncode != 0:
        print("  Test '" + path + "': failed to compile")
        print("  Output:")
        print(result.stdout + result.stderr)
        return

    # Assemble
    result = subprocess.run([nasm_bin] + nasm_args + ["-o", "out.o", "out.s"], capture_output=True, text=True)
    if result.returncode != 0:
        print("  Test '" + path + "': failed to assemble")
        print("  Output:")
        print(result.stdout + result.stderr)
        return

    # Link
    result = subprocess.run([ld_bin] + ld_args + ["-o", "a.out", "out.o"], capture_output=True, text=True)
    if result.returncode != 0:
        print("  Test '" + path + "': failed to link")
        print("  Output:")
        print(result.stdout + result.stderr)
        return

    # Run
    result = subprocess.run(["./a.out"], capture_output=True, text=True)
    if result.returncode != expected_output:
        print("  Test '" + path + "': FAILED")
        print("  Expected return code: " + str(expected_output))
        print("  Got return code: " + str(result.returncode))
        if len(result.stdout) > 0:
            print("  Output: ")
            print(result.stdout + result.stderr)
    else:
        print("  Test '" + path + "': PASSED")

def run_tests(cosec_bin, test_dir):
    printed_header = False
    files = os.listdir(test_dir)
    subdirs = []
    for file in files:
        path = os.path.join(test_dir, file)
        if os.path.isdir(path):
            subdirs.append(path)
            continue # Test subdirectory
        if not os.path.isfile(path):
            continue # Not a file
        _, extension = os.path.splitext(path)
        if extension != ".c":
            continue # Not a .c test file
        if not printed_header:
            print("Testing '" + test_dir + "'")
            printed_header = True
        run_test(cosec_bin, path)

    # Recursively run tests in subdirectories
    for subdir in subdirs:
        run_tests(cosec_bin, subdir)

cosec_bin = sys.argv[1]
test_dir = sys.argv[2]
run_tests(cosec_bin, test_dir)
