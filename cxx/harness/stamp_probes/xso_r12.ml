module type S = sig
  open Set.Make(Bool)
  open Map.Make(Bool)
  type u = int t
end
let x = 1
