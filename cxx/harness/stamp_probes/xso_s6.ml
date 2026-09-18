module type S = sig
  open Set.Make(Bool)
  open Map.Make(Int)
  type u = int t
end
let x = 1
