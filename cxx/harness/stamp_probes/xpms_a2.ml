module type S = sig type t val x : t end
module F (M : S) = struct let y = M.x end
