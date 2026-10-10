// SPDX-License-Identifier: BSD-3-Clause
#include <QAccessible>
#include <QApplication>
#include <QPushButton>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QWidget>
int main(int argc, char **argv) {
	QApplication app(argc, argv);
	QWidget window;
	window.setWindowTitle("Private Qt accessibility repro");
	auto *layout = new QVBoxLayout(&window);
	auto *tree   = new QTreeWidget;
	tree->setColumnCount(3);
	tree->setHeaderLabels({ "Name", "Ping", "Users" });
	tree->setSortingEnabled(true);
	tree->sortItems(1, Qt::AscendingOrder);
	auto *root = new QTreeWidgetItem(tree, { "Favorites" });
	root->setExpanded(true);
	root->setHidden(true);
	auto *lan = new QTreeWidgetItem(tree, { "Synthetic LAN" });
	lan->setExpanded(true);
	new QTreeWidgetItem(lan, { "Private LAN fixture", "0", "0" });
	auto *button = new QPushButton("Add and select private test row");
	layout->addWidget(tree);
	layout->addWidget(button);
	QObject::connect(button, &QPushButton::clicked, &window, [=] {
		auto *child = new QTreeWidgetItem(root, { "Private fixture", "0", "0" });
		root->setHidden(false);
		tree->setCurrentItem(child);
		button->setFocus();
	});
	auto *remove = new QPushButton("Remove one private test row");
	layout->addWidget(remove);
	QObject::connect(remove, &QPushButton::clicked, &window, [=] {
		if (root->childCount() > 0)
			delete root->takeChild(0);
		remove->setFocus();
	});
	auto *clear = new QPushButton("Clear private test rows");
	layout->addWidget(clear);
	QObject::connect(clear, &QPushButton::clicked, &window, [=] {
		qDeleteAll(root->takeChildren());
		root->setHidden(true);
		clear->setFocus();
	});
	auto *removeLan = new QPushButton("Remove synthetic LAN root");
	layout->addWidget(removeLan);
	QObject::connect(removeLan, &QPushButton::clicked, &window, [=] {
		qDeleteAll(tree->findItems("Synthetic LAN", Qt::MatchExactly));
		removeLan->setFocus();
	});
	auto *prime = new QPushButton("Prime accessible cell cache");
	layout->addWidget(prime);
	QObject::connect(prime, &QPushButton::clicked, &window, [=] {
		auto *iface = QAccessible::queryAccessibleInterface(tree);
		auto *table = iface ? iface->tableInterface() : nullptr;
		if (table) {
			for (int row = 0; row < table->rowCount(); ++row)
				for (int column = 0; column < table->columnCount(); ++column)
					table->cellAt(row, column);
		}
		prime->setFocus();
	});
	window.resize(600, 470);
	window.show();
	return app.exec();
}
