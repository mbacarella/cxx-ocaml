module F (X : sig end) = struct
  class type t = object end
  class c = object end
end;;
module M1 = struct end;;
module G (Y : sig end) = struct class type u = F(M1).t end;;
