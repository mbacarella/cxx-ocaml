module F (X : sig end) = struct
  type t = private < m : int; .. > let y = 1 let z = 2
end
