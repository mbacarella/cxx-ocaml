module F (X : sig module N : sig type t = A | B end end) = struct
  let v = X.N.A
end
