module type S = sig
  type u = Set.Make(Bool).t
end
module M : S = struct
  type u = Set.Make(Bool).t
end
