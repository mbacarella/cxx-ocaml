module type E = sig end
module F (X : E) = struct end module _ = F ((val (module Unit : E)))
