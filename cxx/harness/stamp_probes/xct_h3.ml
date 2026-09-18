module F (X : sig end) = struct
  class type t = object end
  class c = object end
end;;
module M1 = struct end;;
module type S = sig class type u = F(M1).t end;;
module N : S = struct class type u = F(M1).t end;;
