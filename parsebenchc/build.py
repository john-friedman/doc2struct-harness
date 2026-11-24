"""Build the independent C executable (Python is a build tool only)."""
if __package__:
    from .standalone.build import main
else:
    from standalone.build import main

if __name__ == '__main__':
    main()
