module Create(P: sig type t end) = struct
    class agent (x : P.t) = object
        method m = x
    end
end
module Basic = struct
    include Create(struct type t = int end)
end
class basic_agent x = object
    inherit Basic.agent x
end
