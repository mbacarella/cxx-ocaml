module type S = sig module N : sig type t val v : t end end
module F (X : S) = struct
  let v = X.N.v
end
