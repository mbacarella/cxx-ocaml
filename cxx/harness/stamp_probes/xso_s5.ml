module type S = sig
  open Set.Make(Bool)
  open Map.Make(Bool)
  type u = Set.Make(Bool).t
  type v = int t
end
let x = 1
