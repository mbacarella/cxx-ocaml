module type T = sig module N : sig type s val x : s end end
module X : T = struct module N = struct type s = int let x = 1 end end
type u = X.N.s
