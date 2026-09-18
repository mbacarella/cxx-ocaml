module type S = sig
  open Set.Make(Bool)
  type u = [ `A of t ]
end
let x = 1
