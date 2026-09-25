module F (X : sig module N : sig type t val v : t end end) = struct
  open X
  let v = N.v
end
