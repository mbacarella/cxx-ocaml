module F (X : sig module N : sig type t = A end end) = struct
  open X
  let v = N.A
end
