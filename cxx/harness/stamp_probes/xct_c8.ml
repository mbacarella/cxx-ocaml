module F (X : sig end) = struct
  class type t = object end
  class c = object end
end;;
module M1 = struct end;;
module P = struct module M1 = struct end end;;
class type u = F(P.M1).t;;
