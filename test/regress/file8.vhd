entity file8 is
end entity;

use std.textio.all;

architecture test of file8 is
    file F: text open read_mode is "NOT_HERE.txt";  -- Error
begin

end architecture;
