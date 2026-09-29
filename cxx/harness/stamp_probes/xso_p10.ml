module type S = sig
  open Set.Make(Bool)
  type u = t
end
module M : S = struct
  type u = Set.Make(Bool).t
end
