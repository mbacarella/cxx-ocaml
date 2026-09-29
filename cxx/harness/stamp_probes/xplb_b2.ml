module F (X : sig type t = A | B end) = struct
  open Either
  let v = Left 1
end
