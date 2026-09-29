module F (X : sig module N : sig type t val v : t end end) = struct
  module N = X.N
  let v = N.v
end
