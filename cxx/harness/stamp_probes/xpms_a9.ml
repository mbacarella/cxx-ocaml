module type S = sig type t val x : t end
module type T = S
module F (M : T) = struct let y = M.x end
