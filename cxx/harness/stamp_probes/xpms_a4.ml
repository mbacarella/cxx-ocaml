module type S = sig type t val x : t end
module F (M : S) (N : S) = struct let y = (M.x, N.x) end
