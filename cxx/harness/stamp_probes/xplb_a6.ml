module F (X : sig type t = A | B end) = struct
  open X
  let v = function A -> 1 | B -> 2
end
