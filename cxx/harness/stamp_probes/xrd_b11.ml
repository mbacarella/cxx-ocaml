module type Profile = sig
    module Priority: sig type t end
    class type c = object method code: Priority.t end
    class virtual d : object val mutable limit_: Priority.t end
end
