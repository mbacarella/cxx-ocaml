module M = struct type t = int let v = 0 end
module type T = module type of M
module N : T = struct type t = int let v = 1 end
module type U = sig type z end
