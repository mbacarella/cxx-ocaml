module F (X : sig type u end) = struct
  type t = X.u list
  let g (x : t) = x
end
module A = struct type u = int end
module B = F (A)
module C = F (struct type u = char end)
let h (x : B.t) = B.g x
