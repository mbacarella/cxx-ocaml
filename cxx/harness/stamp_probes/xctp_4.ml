module M = struct type 'a s = 'a list end
module B = struct class type a = object method a : 'a. 'a M.s -> 'a end end
