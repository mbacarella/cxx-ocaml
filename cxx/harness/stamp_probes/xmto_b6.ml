type z = int
module U = struct type t = int let x = 0 module type HT = sig type t end end
module type T = module type of U
