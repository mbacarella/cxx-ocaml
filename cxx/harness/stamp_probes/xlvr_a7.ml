module M = struct module N = struct type 'a t = 'a list end end
type 'a u = 'a M.N.t
