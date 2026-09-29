module type Profile = sig
    module Priority: sig type t end
    class type c = object method code: Priority.t end
end
