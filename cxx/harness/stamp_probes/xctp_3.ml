module M = struct type 'a s = 'a list end
class type a = object method a : 'a. 'a M.s -> 'a end
