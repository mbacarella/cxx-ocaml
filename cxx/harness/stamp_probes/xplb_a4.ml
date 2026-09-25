module F (X : sig type t = A | B end) = struct
  open X
  let v = A
end
