#include "yql_dq_state.h"

#include <ydb/library/yql/providers/dq/expr_nodes/dqs_expr_nodes.h>
#include <yql/essentials/providers/common/transform/yql_visit.h>
#include <yql/essentials/core/yql_expr_constraint.h>
#include <yql/essentials/ast/yql_constraint.h>

namespace NYql {

using namespace NNodes;

namespace {

class TDqDataSourceConstraintTransformer : public TVisitorTransformerBase {
public:
    TDqDataSourceConstraintTransformer()
        : TVisitorTransformerBase(true)
    {
        AddHandler({TDqReadWrap::CallableName()}, Hndl(&TDqDataSourceConstraintTransformer::CopyFromFirst));
        AddHandler({
            TCoConfigure::CallableName(),
            TDqReadWideWrap::CallableName(),
            TDqReadBlockWideWrap::CallableName(),
            TDqSource::CallableName(),
            TDqSourceWrap::CallableName(),
            TDqSourceWideWrap::CallableName(),
            TDqSourceWideBlockWrap::CallableName(),
            TDqLookupSourceWrap::CallableName(),
            TDqPhyLength::CallableName()
        }, Hndl(&TDqDataSourceConstraintTransformer::HandleDefault));
    }

    TStatus HandleDefault(TExprBase, TExprContext&) {
        return TStatus::Ok;
    }

    TStatus CopyFromFirst(TExprBase node, TExprContext&) {
        node.MutableRaw()->CopyConstraints(node.Raw()->Head());
        return TStatus::Ok;
    }
};

}

THolder<IGraphTransformer> CreateDqDataSourceConstraintTransformer() {
    return THolder<IGraphTransformer>(new TDqDataSourceConstraintTransformer());
}

}
