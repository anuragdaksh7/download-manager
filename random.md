                 URL
                  |
               HEAD
                  |
          Content-Length?
             /       \
           Yes        No
            |          |
        Get Size    Range GET
                       |
                Content-Range?
                   /      \
                 Yes       No
                  |         |
              Get Size   Fallback:
                         Download normally

https://testfile.to/dl/1gb
https://testfile.to/dl/100mb